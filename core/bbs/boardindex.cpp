/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "bbs/boardindex.h"

#include "network/adc/adcbbs.h"

#include <samurai/io/file.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

/** How much of a file to take at a time when reading one. */
constexpr size_t CHUNK = 64 * 1024;

/**
 * The four kinds of record, named by the first character of a line.
 *
 * One file holds all of them because they are all one board's: the key it was
 * made for, the entries the hub has sent, the oldest timestamp the hub says it
 * will replay, and which of the entries the user has read. A reader that does not
 * know a letter skips the line, so a later revision can add one without the files
 * it wrote becoming unreadable to this one.
 *
 * A read record is one of two things and says which by the field it carries: TS
 * is a watermark, everything below that timestamp having been read, and TR is one
 * post read on its own. One kind rather than two because they are one fact stored
 * two ways, and a reader that met a record carrying both would be reading a file
 * this never wrote.
 */
constexpr char RECORD_KEY = 'K';
constexpr char RECORD_ENTRY = 'E';
constexpr char RECORD_OLDEST = 'O';
constexpr char RECORD_READ = 'R';

/**
 * What identifies one board on one hub, and what everything here is kept by.
 *
 * The two halves are joined by a byte that appears in neither - a board name is a
 * subset of ASCII and a hub address has no newline in it - so a hub whose address
 * runs into a board name cannot be taken for another pairing that reads the same
 * way end to end.
 */
std::string key_of(const std::string& hubUrl, const std::string& board)
{
	return hubUrl + '\n' + board;
}

/**
 * A 64 bit FNV-1a of the key, in hex.
 *
 * Enough to separate the boards one client caches, which is hundreds at the very
 * most, and nothing is decided by the value: the file says which key it belongs
 * to, so a collision costs the second board its cache rather than handing it the
 * first one's entries. That is the property that matters, because a hub chooses
 * its own board names and could otherwise aim one at the file of a board on
 * another hub - and this way the index does not drag a cryptographic hash in.
 */
std::string digest_of(const std::string& key)
{
	uint64_t hash = 14695981039346656037ULL;

	for (const char c : key)
	{
		hash ^= static_cast<unsigned char>(c);
		hash *= 1099511628211ULL;
	}

	char out[17];
	snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(hash));

	return std::string(out);
}

/*
 * Escaped the way ADC escapes what it sends, because a cached index is a line
 * based format for the same reason a protocol is: the reader has to be able to
 * find where one record ends without parsing the whole of it.
 */
std::string escape(const std::string& text)
{
	std::string out;
	out.reserve(text.size());

	for (const char c : text)
	{
		switch (c)
		{
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case ' ':  out += "\\s";  break;
			default:   out += c;      break;
		}
	}

	return out;
}

std::string unescape(const std::string& text)
{
	std::string out;
	out.reserve(text.size());

	for (size_t n = 0; n < text.size(); n++)
	{
		if (text[n] != '\\' || n + 1 >= text.size())
		{
			out += text[n];
			continue;
		}

		switch (text[++n])
		{
			case 'n':  out += '\n'; break;
			case 'r':  out += '\r'; break;
			case 's':  out += ' ';  break;
			case '\\': out += '\\'; break;

			/* An escape nobody defined is kept as it was written. */
			default:   out += '\\'; out += text[n]; break;
		}
	}

	return out;
}

std::vector<std::string> split(const std::string& line)
{
	std::vector<std::string> parts;
	size_t at = 0;

	while (at <= line.size())
	{
		const size_t next = line.find(' ', at);

		if (next == std::string::npos)
		{
			parts.push_back(line.substr(at));
			break;
		}

		parts.push_back(line.substr(at, next - at));
		at = next + 1;
	}

	return parts;
}

/** The field named by 'tag', or the empty string when the record has none. */
std::string field(const std::vector<std::string>& parts, const char* tag)
{
	for (const std::string& part : parts)
	{
		if (part.size() >= 2 && part[0] == tag[0] && part[1] == tag[1])
			return unescape(part.substr(2));
	}

	return std::string();
}

uint64_t number(const std::vector<std::string>& parts, const char* tag)
{
	const std::string value = field(parts, tag);
	return value.empty() ? 0 : strtoull(value.c_str(), 0, 10);
}

/** One line of an index file, whichever kind it turned out to be. */
struct Record {
	char type = 0;

	/* RECORD_KEY. */
	std::string hubUrl;
	std::string board;

	/* RECORD_ENTRY. */
	QuickDC::BoardEntry entry;

	/* RECORD_OLDEST. */
	uint64_t oldest = 0;

	/* RECORD_READ: the hash of one post, or the watermark where there is none. */
	std::string read;
	uint64_t below = 0;
};

/**
 * Every record of one index file, in the order it was written.
 *
 * The whole file rather than its end, which is the opposite of what a chat
 * transcript wants and for the opposite reason: a board is read to be shown as a
 * board, so there is no useful tail of one, and the file is bounded by what a hub
 * accepted rather than by how long this client has been running.
 *
 * A record that does not parse is skipped rather than taken as the end of the
 * file. The last one may have been half written when the process went, and a
 * board is not worth losing over its final entry.
 */
std::vector<Record> read_records(const std::string& path, bool* terminated = 0)
{
	std::vector<Record> records;

	Samurai::IO::File file(path);
	if (!file.open(Samurai::IO::File::Mode::Read)) return records;

	std::string content;
	char chunk[CHUNK];
	ssize_t got;

	while ((got = file.read(chunk, sizeof(chunk))) > 0)
		content.append(chunk, static_cast<size_t>(got));

	file.close();

	if (terminated) *terminated = content.empty() || content[content.size() - 1] == '\n';

	size_t at = 0;

	while (at < content.size())
	{
		const size_t end = content.find('\n', at);
		const std::string line = content.substr(at,
			end == std::string::npos ? std::string::npos : end - at);

		at = (end == std::string::npos) ? content.size() : end + 1;

		if (line.size() < 2 || line[1] != ' ') continue;

		const std::vector<std::string> parts = split(line);

		Record record;
		record.type = line[0];

		switch (record.type)
		{
			case RECORD_KEY:
				record.hubUrl = field(parts, "HU");
				record.board = field(parts, "BD");
				break;

			case RECORD_ENTRY:
				record.entry.tth = field(parts, "TR");

				/*
				 * A record whose hash is not a tiger tree root names no document
				 * and could not be fetched, so there is nothing to be gained by
				 * keeping it and a filename to be reached by taking it seriously.
				 */
				if (!ADC::isValidTTH(record.entry.tth)) continue;

				record.entry.timestamp = number(parts, "TS");
				record.entry.size = number(parts, "SI");
				record.entry.cid = field(parts, "ID");
				record.entry.nick = field(parts, "NI");
				record.entry.parent = field(parts, "PA");
				record.entry.thread = field(parts, "TH");
				record.entry.subject = field(parts, "SJ");
				record.entry.removed = field(parts, "RM") == "1";
				break;

			case RECORD_OLDEST:
				record.oldest = number(parts, "OT");
				break;

			case RECORD_READ:
				record.read = field(parts, "TR");

				/* A mark for something that is not a tiger tree root names no post. */
				if (!record.read.empty() && !ADC::isValidTTH(record.read)) continue;

				record.below = number(parts, "TS");
				break;

			default:
				continue;
		}

		records.push_back(record);
	}

	return records;
}

/**
 * Every component of 'directory', so far as it can be made.
 *
 * A level at a time rather than the one mkdir() a chat transcript needs: nothing
 * above a board directory is made as the core starts, so a caller naming
 * "~/.quickdc/boards/index" on a fresh account would otherwise have an unusable
 * index for want of the directory in the middle. A component that is already
 * there is not an error worth acting on, and the answer is whether the last one
 * is a directory once this is done.
 */
bool make_directories(const std::string& directory)
{
	if (directory.empty()) return false;

	const std::string path = Samurai::IO::File::resolvePath(directory);

	for (size_t at = 1; at < path.size(); at++)
	{
		if (path[at] != '/') continue;
		Samurai::IO::File::mkdir(path.substr(0, at).c_str(), 0700);
	}

	Samurai::IO::File::mkdir(path.c_str(), 0700);

	return Samurai::IO::File(path).isDirectory();
}

}

QuickDC::BoardIndex::BoardIndex(const std::string& directory_)
	: directory(directory_)
	, usable(make_directories(directory_))
{
}

bool QuickDC::BoardIndex::isUsable() const
{
	return usable;
}

std::string QuickDC::BoardIndex::pathFor(const std::string& hubUrl,
                                         const std::string& board) const
{
	/* The digest of the key and nothing taken from it; see key_of(). */
	return directory + "/" + digest_of(key_of(hubUrl, board)) + ".idx";
}

QuickDC::BoardIndex::Cached&
QuickDC::BoardIndex::remember(const std::string& hubUrl,
                              const std::string& board) const
{
	const std::string key = key_of(hubUrl, board);

	const std::map<std::string, Cached>::iterator known = cached.find(key);
	if (known != cached.end()) return known->second;

	Cached& state = cached[key];

	for (const Record& record : read_records(pathFor(hubUrl, board), &state.terminated))
	{
		switch (record.type)
		{
			case RECORD_KEY:
				state.keyed = true;
				if (record.hubUrl != hubUrl || record.board != board)
					state.foreign = true;
				break;

			case RECORD_ENTRY:
			{
				uint64_t& held = state.held[record.entry.tth];

				/* Whichever record for a hash is the newest is the one that counts. */
				if (record.entry.timestamp >= held)
				{
					held = record.entry.timestamp;

					/* Including whether it is the withdrawal of the post, by the
					   same rule and for the same reason. */
					if (record.entry.removed)
						state.withdrawn.insert(record.entry.tth);
					else
						state.withdrawn.erase(record.entry.tth);
				}

				if (record.entry.timestamp > state.highest)
					state.highest = record.entry.timestamp;
				break;
			}

			case RECORD_OLDEST:
				state.described = true;
				state.oldest = record.oldest;
				break;

			case RECORD_READ:
				/*
				 * A mark for one post where the record names one, and otherwise the
				 * watermark - which only ever moves up, so the records may be read
				 * in any order and a file holding several says the newest thing.
				 */
				if (!record.read.empty())
					state.readByHash.insert(record.read);
				else if (record.below > state.readBelow)
					state.readBelow = record.below;
				break;

			default:
				break;
		}
	}

	/*
	 * Somebody else's file, so nothing is read from it either: entries written
	 * for another hub's board must not be reported as this one's, which is the
	 * whole reason the key is in the file.
	 */
	if (state.foreign)
	{
		state.held.clear();
		state.highest = 0;
		state.withdrawn.clear();
		state.readBelow = 0;
		state.readByHash.clear();
		state.described = false;
		state.oldest = 0;
	}

	return state;
}

bool QuickDC::BoardIndex::Cached::wasRead(const std::string& tth) const
{
	if (readByHash.find(tth) != readByHash.end()) return true;

	const std::map<std::string, uint64_t>::const_iterator entry = held.find(tth);

	/*
	 * Strictly below the watermark, which is the whole of the rule: an entry
	 * standing at that second is one the watermark was written without, since the
	 * hub may accept another post in the same second and a boundary that took in
	 * that second would mark as read a post nobody has seen.
	 *
	 * A hash no entry is held for is therefore read only by a mark of its own.
	 * There is nothing to hold against the watermark, and the watermark is a claim
	 * about the entries of a board rather than about hashes.
	 */
	return entry != held.end() && entry->second < readBelow;
}

bool QuickDC::BoardIndex::write(const std::string& path, Cached& state,
                                const std::string& hubUrl, const std::string& board,
                                const std::string& record)
{
	Samurai::IO::File file(path);

	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Append))
		return false;

	/*
	 * A newline where the last write did not manage one, so that the record about
	 * to be written does not join onto a half written one and be lost with it.
	 */
	if (!state.terminated)
	{
		if (file.write("\n", 1) < 0)
		{
			file.close();
			return false;
		}

		state.terminated = true;
	}

	/*
	 * The key goes in before anything else is written to a file that has none,
	 * which is a file this client has just made - or one truncated to nothing by
	 * a filesystem that lost it. Its position is not part of the format: a reader
	 * looks for it anywhere, so a key written into the middle of a file that
	 * somehow lacks one still says what that file is.
	 */
	if (!state.keyed)
	{
		const std::string key = std::string(1, RECORD_KEY) + " HU" + escape(hubUrl)
			+ " BD" + escape(board) + "\n";

		if (file.write(key.data(), key.size()) < 0)
		{
			file.close();
			return false;
		}

		state.keyed = true;
	}

	const bool ok = file.write(record.data(), record.size()) >= 0;
	file.close();

	return ok;
}

bool QuickDC::BoardIndex::append(const std::string& hubUrl, const std::string& board,
                                 const BoardEntry& entry)
{
	/*
	 * Nothing is kept, so nothing can be a repeat of anything - and a caller
	 * deciding whether to show an entry must not be told to drop a live one.
	 */
	if (!usable) return true;

	/* Neither storable nor fetchable, and it must not reach a filename. */
	if (!ADC::isValidTTH(entry.tth)) return false;

	const std::string path = pathFor(hubUrl, board);
	Cached& state = remember(hubUrl, board);

	if (state.foreign) return true;

	const std::map<std::string, uint64_t>::iterator held = state.held.find(entry.tth);

	/*
	 * One rule for the two ways an entry arrives twice. A duplicate from the
	 * resumption window carries the timestamp it carried before and is discarded;
	 * a tombstone carries the time of the withdrawal, which is greater than
	 * anything on the board, and replaces what is held.
	 */
	if (held != state.held.end() && entry.timestamp <= held->second) return false;

	std::string out = std::string(1, RECORD_ENTRY) + " TR" + escape(entry.tth)
		+ " TS" + std::to_string(entry.timestamp);

	if (entry.size)             out += " SI" + std::to_string(entry.size);
	if (!entry.cid.empty())     out += " ID" + escape(entry.cid);
	if (!entry.nick.empty())    out += " NI" + escape(entry.nick);
	if (!entry.parent.empty())  out += " PA" + escape(entry.parent);
	if (!entry.thread.empty())  out += " TH" + escape(entry.thread);
	if (!entry.subject.empty()) out += " SJ" + escape(entry.subject);
	if (entry.removed)          out += " RM1";

	out += "\n";

	if (!write(path, state, hubUrl, board, out)) return true;

	state.held[entry.tth] = entry.timestamp;
	if (entry.timestamp > state.highest) state.highest = entry.timestamp;

	/* And what unread() needs of it: whether the newest record is a withdrawal. */
	if (entry.removed)
		state.withdrawn.insert(entry.tth);
	else
		state.withdrawn.erase(entry.tth);

	return true;
}

std::vector<QuickDC::BoardEntry>
QuickDC::BoardIndex::entries(const std::string& hubUrl,
                             const std::string& board) const
{
	std::vector<BoardEntry> found;
	if (!usable) return found;

	const std::string path = pathFor(hubUrl, board);
	if (remember(hubUrl, board).foreign) return found;

	/*
	 * Folded by hash as the file is walked, by the same rule append() writes it
	 * with: the newest record for a hash is the one that counts, so a tombstone
	 * takes the place of the entry it withdraws rather than appearing beside it.
	 */
	std::map<std::string, size_t> at;

	for (const Record& record : read_records(path))
	{
		if (record.type != RECORD_ENTRY) continue;

		const std::map<std::string, size_t>::iterator known = at.find(record.entry.tth);

		if (known == at.end())
		{
			at[record.entry.tth] = found.size();
			found.push_back(record.entry);
			found.back().board = board;
			continue;
		}

		if (record.entry.timestamp > found[known->second].timestamp)
		{
			found[known->second] = record.entry;
			found[known->second].board = board;
		}
	}

	/*
	 * By the timestamp the hub assigned rather than the order the records were
	 * written, which is where a tombstone moves to the head of the board and a
	 * resumed entry falls back into its place. Stable, so that entries sharing a
	 * timestamp keep the order they arrived in and a thread does not rearrange
	 * itself between sessions.
	 */
	std::stable_sort(found.begin(), found.end(),
		[](const BoardEntry& a, const BoardEntry& b) { return a.timestamp < b.timestamp; });

	return found;
}

uint64_t QuickDC::BoardIndex::cursor(const std::string& hubUrl,
                                     const std::string& board) const
{
	if (!usable) return 0;

	return remember(hubUrl, board).highest;
}

bool QuickDC::BoardIndex::markRead(const std::string& hubUrl, const std::string& board,
                                   const std::string& tth)
{
	/* Nothing is kept, so nothing it is asked about will ever have changed. */
	if (!usable) return false;

	/* Not a post, so not something a reading pane could have opened. */
	if (!ADC::isValidTTH(tth)) return false;

	const std::string path = pathFor(hubUrl, board);
	Cached& state = remember(hubUrl, board);

	if (state.foreign) return false;

	if (state.wasRead(tth)) return false;

	const std::string out = std::string(1, RECORD_READ) + " TR" + escape(tth) + "\n";

	if (!write(path, state, hubUrl, board, out)) return false;

	state.readByHash.insert(tth);

	return true;
}

void QuickDC::BoardIndex::markAllRead(const std::string& hubUrl,
                                      const std::string& board)
{
	if (!usable) return;

	const std::string path = pathFor(hubUrl, board);
	Cached& state = remember(hubUrl, board);

	if (state.foreign) return;

	std::string out;

	/*
	 * Below the highest timestamp held and not below one past it; see the note on
	 * markAllRead(). Only where it moves, as describe() writes only where OT moved:
	 * a board dismissed twice with nothing arriving in between says nothing new.
	 */
	const bool moves = state.highest > state.readBelow;

	if (moves)
		out += std::string(1, RECORD_READ) + " TS"
			+ std::to_string(state.highest) + "\n";

	/*
	 * And the entries standing at that second one at a time, since the watermark
	 * deliberately does not reach them. A tombstone is not among them: unread()
	 * does not count one, and nobody read a post that is no longer on the board.
	 */
	std::vector<std::string> standing;

	for (const std::pair<const std::string, uint64_t>& entry : state.held)
	{
		if (entry.second != state.highest) continue;
		if (state.withdrawn.find(entry.first) != state.withdrawn.end()) continue;
		if (state.wasRead(entry.first)) continue;

		standing.push_back(entry.first);
		out += std::string(1, RECORD_READ) + " TR" + escape(entry.first) + "\n";
	}

	if (out.empty()) return;

	/*
	 * One append for the watermark and the marks of that second together, which is
	 * also the safe order to lose half of: the watermark does not reach the second
	 * it names, so a write torn after it leaves those entries unread rather than
	 * leaving them read without anybody having read them.
	 */
	if (!write(path, state, hubUrl, board, out)) return;

	if (moves) state.readBelow = state.highest;

	for (const std::string& tth : standing)
		state.readByHash.insert(tth);
}

bool QuickDC::BoardIndex::isRead(const std::string& hubUrl, const std::string& board,
                                 const std::string& tth) const
{
	if (!usable) return false;

	const Cached& state = remember(hubUrl, board);

	return !state.foreign && state.wasRead(tth);
}

size_t QuickDC::BoardIndex::unread(const std::string& hubUrl,
                                   const std::string& board) const
{
	if (!usable) return 0;

	const Cached& state = remember(hubUrl, board);

	if (state.foreign) return 0;

	size_t count = 0;

	/*
	 * Counted rather than kept, so that it cannot drift out of step with the
	 * entries it is counting and cannot go negative however arrivals and markings
	 * interleave. A tombstone is not an entry a badge may ask to have opened.
	 */
	for (const std::pair<const std::string, uint64_t>& entry : state.held)
	{
		if (state.withdrawn.find(entry.first) != state.withdrawn.end()) continue;
		if (state.wasRead(entry.first)) continue;

		count++;
	}

	return count;
}

void QuickDC::BoardIndex::describe(const std::string& hubUrl, const Board& board)
{
	if (!usable) return;

	const std::string path = pathFor(hubUrl, board.name);
	Cached& state = remember(hubUrl, board.name);

	if (state.foreign) return;

	/*
	 * Only where the value has moved. A hub sends a fresh descriptor whenever any
	 * field of a board changes, and most of those changes are a post count; a
	 * record for each of them would be a file that grows without saying anything
	 * new.
	 */
	if (state.described && state.oldest == board.oldest) return;

	const std::string out = std::string(1, RECORD_OLDEST) + " OT"
		+ std::to_string(board.oldest) + "\n";

	if (!write(path, state, hubUrl, board.name, out)) return;

	state.described = true;
	state.oldest = board.oldest;
}

QuickDC::BoardIndex::View QuickDC::BoardIndex::view(const std::string& hubUrl,
                                                    const std::string& board) const
{
	if (!usable) return View::Unknown;

	const Cached& state = remember(hubUrl, board);

	if (!state.described) return View::Unknown;

	return state.highest < state.oldest ? View::Gap : View::Complete;
}

bool QuickDC::BoardIndex::contains(const std::string& hubUrl,
                                   const std::string& board) const
{
	if (!usable) return false;

	const std::string path = pathFor(hubUrl, board);

	return !remember(hubUrl, board).foreign
		&& Samurai::IO::File::exists(path.c_str());
}

void QuickDC::BoardIndex::forget(const std::string& hubUrl, const std::string& board)
{
	if (!usable) return;

	/* Not a file belonging to another key, which forgetting this board is not. */
	if (remember(hubUrl, board).foreign) return;

	Samurai::IO::File(pathFor(hubUrl, board)).remove();

	/* Or the entries just thrown away would go on being recognised as duplicates. */
	cached.erase(key_of(hubUrl, board));
}
