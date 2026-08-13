/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include "config/preferences.h"
#include "debug/dbg.h"
#include "network/adc/adcbbs.h"
#include "network/adc/adccommon.h"
#include "network/adc/parser.h"
#include "network/adc/server/boards.h"

#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/file.h>

#include <algorithm>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

namespace {

/**
 * The configuration section a board is defined by, less its name.
 *
 * A section per board, so that writing one down is the whole of creating it and
 * there is no second list to keep in step. Everything after the prefix is the
 * board's name and is held to ADC::isValidBoardName() like a name off the wire -
 * the file is the operator's own, but a name that is not a board name would
 * reach a descriptor, a subscription and every client that read one.
 */
const char BOARD_SECTION_PREFIX[] = "Local hub board ";

/** How much of a file to take at a time when reading one back. */
constexpr size_t CHUNK = 64 * 1024;

/**
 * A configured number, held to a range it can be used as.
 *
 * Every one of these is an int from a file the hub does not write and is used as
 * an unsigned bound, which is the mistake the hub's own settings are clamped
 * against: a configured 0 for a board's entry limit would make 'entries.size() >
 * maxEntries' a comparison against something no board could ever reach, and the
 * index would grow for as long as the hub ran.
 */
uint64_t clamped(int value, int low, int high)
{
	return static_cast<uint64_t>(std::clamp(value, low, high));
}

/** Whether 'text' begins with 'prefix'. */
bool startsWith(const std::string& text, const char* prefix)
{
	const size_t length = strlen(prefix);
	return text.size() > length && text.compare(0, length, prefix) == 0;
}

}


ADC::Command* ADC::LocalHub::writeEntry(const QuickDC::BoardEntry& entry)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('I','B','B','L'));

	/*
	 * A tombstone carries the hash, the board, the timestamp and the flag, and
	 * nothing else: the subject and the author of a withdrawn post are usually the
	 * reason it was withdrawn, so repeating them in the record of its removal
	 * would publish the thing being taken back.
	 */
	if (entry.removed)
	{
		cmd->addArgument("TR", entry.tth);
		cmd->addArgument("BD", entry.board);
		cmd->addArgument("TS", std::to_string(entry.timestamp));
		cmd->addArgument("RM", "1");
		return cmd;
	}

	cmd->addArgument("TR", entry.tth);
	cmd->addArgument("SI", std::to_string(entry.size));
	cmd->addArgument("BD", entry.board);
	cmd->addArgument("ID", entry.cid);
	if (!entry.nick.empty()) cmd->addArgument("NI", entry.nick);
	if (!entry.parent.empty()) cmd->addArgument("PA", entry.parent);
	cmd->addArgument("TH", entry.thread);
	if (!entry.subject.empty()) cmd->addArgument("SJ", entry.subject);
	cmd->addArgument("TS", std::to_string(entry.timestamp));

	return cmd;
}


ADC::Command* ADC::LocalHub::writeBoard(const QuickDC::Board& board)
{
	ADC::Command* cmd = new ADC::Command(FOURCC('I','B','B','D'));

	cmd->addArgument("BD", board.name);

	/* As a tombstone is: the name and the fact that it is gone. The limits and
	   the timestamps of a board that no longer exists describe nothing. */
	if (board.removed)
	{
		cmd->addArgument("RM", "1");
		return cmd;
	}

	if (!board.title.empty()) cmd->addArgument("NI", board.title);
	if (!board.description.empty()) cmd->addArgument("DE", board.description);
	cmd->addArgument("PE", std::to_string(board.permissions));
	cmd->addArgument("MS", std::to_string(board.maxSize));
	cmd->addArgument("TS", std::to_string(board.newest));
	cmd->addArgument("OT", std::to_string(board.oldest));
	cmd->addArgument("NP", std::to_string(board.posts));

	return cmd;
}


ADC::LocalHub::Allowance::Allowance(double rate_, double burst_)
	: rate(rate_)
	, burst(burst_)
	, tokens(burst_)
	, filled(std::chrono::steady_clock::now())
{

}

bool ADC::LocalHub::Allowance::spend(double cost)
{
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	const std::chrono::duration<double> elapsed = now - filled;
	filled = now;

	tokens += elapsed.count() * rate;
	if (tokens > burst) tokens = burst;

	if (tokens < cost) return false;

	tokens -= cost;
	return true;
}

bool ADC::LocalHub::Allowance::affords(double cost) const
{
	const std::chrono::duration<double> elapsed =
		std::chrono::steady_clock::now() - filled;

	return std::min(burst, tokens + elapsed.count() * rate) >= cost;
}

uint64_t ADC::LocalHub::Allowance::retryAfter(double cost) const
{
	/*
	 * Const, so the bucket is not filled here - what has accrued since the last
	 * spend is worked out and left where it is. Anything the caller could not
	 * afford is being reported on, so this is only ever asked immediately after a
	 * spend that failed, and the difference is a fraction of a second.
	 */
	const std::chrono::duration<double> elapsed =
		std::chrono::steady_clock::now() - filled;

	double available = tokens + elapsed.count() * rate;
	if (available > burst) available = burst;

	if (available >= cost) return 1;

	/* A cost past the whole bucket is never affordable, and saying so as a wait
	   of one second would have a client retry into the same refusal for ever. */
	if (cost > burst) return 0;

	const double wait = (cost - available) / rate;
	return static_cast<uint64_t>(wait) + 1;
}


ADC::LocalHub::Subscriptions::Subscriptions()
	: requests(BOARD_REQUEST_RATE, BOARD_REQUEST_BURST)
	, replays(BOARD_REPLAY_RATE, BOARD_REPLAY_BURST)
	, posts(BOARD_POST_RATE, BOARD_POST_BURST)
	, threads(BOARD_THREAD_RATE, BOARD_THREAD_BURST)
{

}

bool ADC::LocalHub::Subscriptions::has(const std::string& board) const
{
	return cursors.count(board) != 0;
}

size_t ADC::LocalHub::Subscriptions::count() const
{
	return cursors.size();
}

bool ADC::LocalHub::Subscriptions::subscribe(const std::string& board, uint64_t cursor)
{
	/* Replacing one this session already holds adds nothing, so it is allowed at
	   the ceiling: a second BBL naming the same board is a replacement. */
	std::map<std::string, uint64_t>::iterator it = cursors.find(board);
	if (it != cursors.end())
	{
		(*it).second = cursor;
		return true;
	}

	if (cursors.size() >= MAX_SUBSCRIPTIONS_PER_SESSION) return false;

	cursors.insert(std::pair<std::string, uint64_t>(board, cursor));
	return true;
}

void ADC::LocalHub::Subscriptions::cancel(const std::string& board)
{
	cursors.erase(board);
}

void ADC::LocalHub::Subscriptions::clear()
{
	cursors.clear();
}

void ADC::LocalHub::Subscriptions::noteDescribed(const std::string& board)
{
	described.insert(board);
}

bool ADC::LocalHub::Subscriptions::wasDescribed(const std::string& board) const
{
	return described.count(board) != 0;
}

void ADC::LocalHub::Subscriptions::forget(const std::string& board)
{
	cursors.erase(board);
	described.erase(board);
}

std::vector<std::string> ADC::LocalHub::Subscriptions::boards() const
{
	std::vector<std::string> names;
	names.reserve(cursors.size());

	for (const std::pair<const std::string, uint64_t>& cursor : cursors)
		names.push_back(cursor.first);

	return names;
}

uint64_t ADC::LocalHub::Subscriptions::cursor(const std::string& board) const
{
	std::map<std::string, uint64_t>::const_iterator it = cursors.find(board);
	return it == cursors.end() ? 0 : (*it).second;
}

void ADC::LocalHub::Subscriptions::advance(const std::string& board, uint64_t timestamp)
{
	std::map<std::string, uint64_t>::iterator it = cursors.find(board);
	if (it == cursors.end()) return;

	if (timestamp > (*it).second) (*it).second = timestamp;
}


ADC::LocalHub::BoardStore::BoardStore(const std::string& directory_)
	: directory(directory_)
	, usable(false)
{
	if (directory.empty()) return;

	/* One level, as the chat archive does it: everything above this is made by
	   Core as it starts, and a directory that is already there is not an error. */
	Samurai::IO::File::mkdir(directory.c_str(), 0700);

	usable = Samurai::IO::File(directory).isDirectory();

	if (!usable)
		QERR("The bulletin board directory '%s' is not usable; this hub will not "
			"announce " ADC_EXT_BULLETIN, directory.c_str());
}

ADC::LocalHub::BoardStore::~BoardStore()
{

}

bool ADC::LocalHub::BoardStore::isUsable() const
{
	return usable;
}

size_t ADC::LocalHub::BoardStore::count() const
{
	return boards.size();
}

uint64_t ADC::LocalHub::BoardStore::now() const
{
	const time_t seconds = time(0);
	return seconds > 0 ? static_cast<uint64_t>(seconds) : 0;
}

std::vector<std::string> ADC::LocalHub::BoardStore::names() const
{
	std::vector<std::string> found;
	found.reserve(boards.size());

	for (const BoardState& board : boards)
		found.push_back(board.name);

	return found;
}

ADC::LocalHub::BoardStore::BoardState*
ADC::LocalHub::BoardStore::find(const std::string& name)
{
	for (BoardState& board : boards)
		if (board.name == name) return &board;

	return 0;
}

const ADC::LocalHub::BoardStore::BoardState*
ADC::LocalHub::BoardStore::find(const std::string& name) const
{
	for (const BoardState& board : boards)
		if (board.name == name) return &board;

	return 0;
}

bool ADC::LocalHub::BoardStore::exists(const std::string& board) const
{
	return find(board) != 0;
}

bool ADC::LocalHub::BoardStore::registeredOnly(const std::string& board) const
{
	const BoardState* state = find(board);
	return state && state->registeredOnly;
}

uint64_t ADC::LocalHub::BoardStore::permissionsFor(const std::string& board,
                                                   Credentials credentials) const
{
	const BoardState* state = find(board);
	if (!state) return 0;

	/*
	 * A board reserved for registered sessions is closed to one that has
	 * authenticated as nothing, whatever its mask says. This hub knows three
	 * credentials and none of them is 'registered' in the sense the extension's
	 * code 26 means, so an operator or an administrator is what stands in for it:
	 * those are the sessions that presented something.
	 */
	if (state->registeredOnly && credentials == Cred_User) return 0;

	switch (credentials)
	{
		case Cred_Operator: return state->operatorPermissions;
		case Cred_Admin:    return state->adminPermissions;
		case Cred_User:     break;
	}

	return state->userPermissions;
}

bool ADC::LocalHub::BoardStore::describe(const std::string& board,
                                         Credentials credentials,
                                         QuickDC::Board& out) const
{
	const BoardState* state = find(board);
	if (!state) return false;

	QuickDC::Board described;
	described.name = state->name;
	described.title = state->title;
	described.description = state->description;
	described.permissions = permissionsFor(board, credentials);
	described.maxSize = state->maxSize;
	described.oldest = state->oldest;

	/* TS is the newest entry's, and 0 for a board with none - not the highest
	   timestamp ever handed out here, which a withdrawal moves on its own. */
	described.newest = state->entries.empty() ? 0 : state->entries.back().timestamp;

	described.posts = 0;
	for (const QuickDC::BoardEntry& entry : state->entries)
		if (!entry.removed) described.posts++;

	out = std::move(described);
	return true;
}

const QuickDC::BoardEntry*
ADC::LocalHub::BoardStore::findEntry(const std::string& board,
                                     const std::string& tth) const
{
	const BoardState* state = find(board);
	if (!state) return 0;

	for (const QuickDC::BoardEntry& entry : state->entries)
		if (entry.tth == tth) return &entry;

	return 0;
}

std::vector<QuickDC::BoardEntry>
ADC::LocalHub::BoardStore::since(const std::string& board, uint64_t from) const
{
	std::vector<QuickDC::BoardEntry> found;

	const BoardState* state = find(board);
	if (!state) return found;

	/* A request from before what the board will replay is a request from there. */
	if (from < state->oldest) from = state->oldest;

	for (const QuickDC::BoardEntry& entry : state->entries)
		if (entry.timestamp >= from) found.push_back(entry);

	return found;
}

ADC::LocalHub::BoardStore::Outcome
ADC::LocalHub::BoardStore::add(const std::string& board,
                               const QuickDC::BoardEntry& post,
                               QuickDC::BoardEntry& out)
{
	BoardState* state = find(board);
	if (!state) return Outcome::NoBoard;

	uint64_t byAuthor = 0;
	for (const QuickDC::BoardEntry& entry : state->entries)
	{
		if (entry.tth == post.tth) return Outcome::Duplicate;
		if (!entry.removed && entry.cid == post.cid) byAuthor++;
	}

	if (byAuthor >= state->maxEntriesPerAuthor) return Outcome::AuthorLimit;

	QuickDC::BoardEntry entry;
	entry.tth = post.tth;
	entry.size = post.size;
	entry.board = state->name;
	entry.cid = post.cid;
	entry.nick = post.nick;
	entry.parent = post.parent;
	entry.subject = post.subject;
	entry.removed = false;

	/*
	 * The three monotonicity rules, in one line each way they can be read: never
	 * below the highest already handed out here, the highest reused where the
	 * clock has moved backwards, and so never in the past. An entry stamped behind
	 * a cursor is an entry delivered to nobody.
	 */
	entry.timestamp = std::max(now(), state->highest);
	state->highest = entry.timestamp;

	/*
	 * The thread is the parent's, and the post's own hash where there is no parent
	 * or where the parent is not indexed here. A reply to a post on another hub -
	 * or to one that has since been withdrawn - starts a thread as far as this
	 * board is concerned, there being no thread here to attach it to.
	 */
	entry.thread = entry.tth;
	if (!entry.parent.empty())
	{
		const QuickDC::BoardEntry* parent = findEntry(board, entry.parent);
		if (parent && !parent->removed && !parent->thread.empty())
			entry.thread = parent->thread;
	}

	state->entries.push_back(entry);

	/* Appended first and the board trimmed after, so the ordinary case is one
	   write at the end of a file rather than the file written out again. */
	append(*state, entry);
	if (trim(*state)) rewrite(*state);

	out = entry;
	return Outcome::Accepted;
}

bool ADC::LocalHub::BoardStore::withdraw(const std::string& board,
                                         const std::string& tth,
                                         QuickDC::BoardEntry& tombstone)
{
	BoardState* state = find(board);
	if (!state) return false;

	std::vector<QuickDC::BoardEntry>::iterator it = state->entries.begin();
	while (it != state->entries.end() && (*it).tth != tth) it++;

	if (it == state->entries.end()) return false;
	if ((*it).removed) return false;

	const uint64_t was = (*it).timestamp;
	state->entries.erase(it);

	QuickDC::BoardEntry stone;
	stone.tth = tth;
	stone.board = state->name;
	stone.removed = true;

	/*
	 * A new timestamp, and this is what makes withdrawal work: it puts the
	 * tombstone at the head of the stream, so it reaches a subscriber at once and
	 * an absent client the moment it resumes. The original timestamp would have
	 * put it behind every cursor that had already moved past the entry.
	 *
	 * Strictly past the entry it replaces, and not merely 'the time of the
	 * withdrawal'. A client holding an entry replaces it when one arrives for the
	 * same hash carrying a greater timestamp and ignores it otherwise - which is
	 * the one rule that lets it discard the duplicates a resumption re-sends - so a
	 * post withdrawn in the same second it was accepted would be a tombstone every
	 * client that held the post was obliged to throw away. A second's clock cannot
	 * express 'later' inside a second, so the hub says it by adding one.
	 */
	stone.timestamp = std::max(now(), state->highest);
	if (stone.timestamp <= was) stone.timestamp = was + 1;

	state->highest = stone.timestamp;

	state->entries.push_back(stone);

	/* The whole file, because the entry being withdrawn has to stop being in it. */
	rewrite(*state);

	tombstone = stone;
	return true;
}

bool ADC::LocalHub::BoardStore::trim(BoardState& board)
{
	if (board.entries.size() <= board.maxEntries) return false;

	const size_t drop = board.entries.size() - static_cast<size_t>(board.maxEntries);
	board.entries.erase(board.entries.begin(),
		board.entries.begin() + static_cast<std::ptrdiff_t>(drop));

	/*
	 * And the board says so. OT is where a hub that expires its oldest entries
	 * declares what it will still replay, and it is the only warning a client with
	 * an older cursor gets that what it holds has a gap no subscription will close.
	 */
	board.oldest = board.entries.empty() ? board.highest : board.entries.front().timestamp;

	return true;
}

std::string ADC::LocalHub::BoardStore::pathFor(const std::string& board) const
{
	/*
	 * The name is hashed and never used, because a board name is protocol text:
	 * the character set excludes the separators but permits '.', so ".." is a
	 * legal board name and so is "...". isValidBoardName() is applied to every
	 * name that gets this far, and this does not depend on that having been right -
	 * a validation that was wrong somewhere else must not be the thing standing
	 * between a configuration file and the rest of the disk.
	 */
	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(board.data(), board.size());

	char name[64] = {0,};
	if (!tiger.digest().getFormattedString(
		Samurai::Crypto::Digest::HashValue::Format::Base32, name, sizeof(name)))
		return std::string();

	return directory + "/" + name + ".idx";
}

void ADC::LocalHub::BoardStore::append(const BoardState& board,
                                       const QuickDC::BoardEntry& entry)
{
	if (!usable) return;

	const std::string path = pathFor(board.name);
	if (path.empty()) return;

	/* Owned here: nothing queues this one, so nothing else will free it. */
	ADC::Command* line = writeEntry(entry);
	const std::string raw(line->getRawCommand());
	delete line;

	Samurai::IO::File file(path);
	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Append))
	{
		QERR("Could not write the index of board '%s'", board.name.c_str());
		return;
	}

	file.write(raw.data(), raw.size());
	file.close();
}

void ADC::LocalHub::BoardStore::rewrite(const BoardState& board)
{
	if (!usable) return;

	const std::string path = pathFor(board.name);
	if (path.empty()) return;

	/*
	 * Written beside the index and moved onto it, rather than the index being
	 * truncated and filled in place. What survives a crash halfway through a
	 * truncating write is the front of the file, which is the oldest entries -
	 * so the one thing lost would be everything recent.
	 */
	const std::string temporary = path + ".tmp";

	Samurai::IO::File file(temporary);
	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate))
	{
		QERR("Could not rewrite the index of board '%s'", board.name.c_str());
		return;
	}

	for (const QuickDC::BoardEntry& entry : board.entries)
	{
		ADC::Command* line = writeEntry(entry);
		const std::string raw(line->getRawCommand());
		delete line;

		file.write(raw.data(), raw.size());
	}

	file.close();

	if (!file.rename(path))
	{
		QERR("Could not replace the index of board '%s'", board.name.c_str());
		Samurai::IO::File(temporary).remove();
	}
}

void ADC::LocalHub::BoardStore::load(BoardState& board)
{
	if (!usable) return;

	const std::string path = pathFor(board.name);
	if (path.empty()) return;

	Samurai::IO::File file(path);
	if (!file.open(Samurai::IO::File::Mode::Read)) return;

	const off_t size = file.size();
	if (size <= 0)
	{
		file.close();
		return;
	}

	/*
	 * The tail rather than the head of a file past the bound: the newest entries
	 * are the ones a subscriber wants and the oldest are the ones the board would
	 * drop anyway. Whatever partial line the seek lands in the middle of fails to
	 * parse and is skipped, which is the same treatment a truncated file gets.
	 *
	 * Bounded by what the board is configured to hold as well as absolutely, so
	 * that the work of starting up follows the board an operator asked for rather
	 * than whatever is on the disk. A line is a couple of hundred bytes, so a
	 * kilobyte an entry is generous and still bounds what is read and held.
	 */
	const size_t bound = std::min<size_t>(MAX_INDEX_FILE,
		static_cast<size_t>(std::min<uint64_t>(board.maxEntries, MAX_INDEX_FILE)) * 1024);

	size_t want = static_cast<size_t>(size);
	if (want > bound)
	{
		QERR("The index of board '%s' is %llu bytes; reading the last %zu",
			board.name.c_str(), static_cast<unsigned long long>(size), bound);

		if (!file.seek(size - static_cast<off_t>(bound)))
		{
			file.close();
			return;
		}
		want = bound;
	}

	std::string content;
	content.reserve(want);

	while (content.size() < want)
	{
		char buf[CHUNK];
		const size_t ask = std::min(sizeof(buf), want - content.size());
		const ssize_t got = file.read(buf, ask);
		if (got <= 0) break;
		content.append(buf, static_cast<size_t>(got));
	}

	file.close();

	size_t lines = 0;
	std::vector<QuickDC::BoardEntry> read;

	/*
	 * Where each hash landed, so that recognising one already read is a lookup
	 * rather than a walk. A file is bounded but not small, and a walk per line
	 * would make reading one cost the square of its length - which is a length
	 * decided by whatever is on the disk.
	 */
	std::map<std::string, size_t> seen;

	size_t at = 0;
	while (at < content.size())
	{
		const size_t end = content.find('\n', at);
		const size_t length = (end == std::string::npos ? content.size() : end) - at;
		const std::string line = content.substr(at, length);
		at += length + 1;

		if (line.empty()) continue;
		lines++;

		/*
		 * One whole ADC message per line, read back with the reader a client reads
		 * a BBL with. A line that does not parse, or that parses into something
		 * that is not an entry of this board, is skipped: a file that has been
		 * truncated or filled with something else costs the entries that cannot be
		 * read and does not cost the hub.
		 */
		ADC::Command cmd;
		if (!ADC::Parser::parse(line.c_str(), line.size(), cmd)) continue;
		if (cmd.getFourcc() != FOURCC('I','B','B','L')) continue;

		QuickDC::BoardEntry entry;
		if (!ADC::readEntry(&cmd, entry)) continue;
		if (entry.board != board.name) continue;

		/* Later in the file wins, which is how a tombstone appended after the
		   entry it withdraws takes effect on the way back in. */
		std::map<std::string, size_t>::iterator held = seen.find(entry.tth);
		if (held != seen.end())
		{
			read[(*held).second] = entry;
			continue;
		}

		seen.insert(std::pair<std::string, size_t>(entry.tth, read.size()));
		read.push_back(entry);
	}

	/*
	 * Sorted rather than trusted to be in order. The hub only ever appends in
	 * non-decreasing order of timestamp, but the file is whatever is on disk, and
	 * entries have to leave here in the order a subscriber may be sent them.
	 */
	std::stable_sort(read.begin(), read.end(),
		[](const QuickDC::BoardEntry& a, const QuickDC::BoardEntry& b) {
			return a.timestamp < b.timestamp;
		});

	board.entries = std::move(read);

	for (const QuickDC::BoardEntry& entry : board.entries)
		if (entry.timestamp > board.highest) board.highest = entry.timestamp;

	const size_t kept = board.entries.size();

	/*
	 * Trimming here decides OT for a board that was already at its bound when the
	 * hub last ran: what it holds is what it will replay, and a client whose
	 * cursor is older than that has a gap whether or not this process is the one
	 * that dropped the entries.
	 */
	const bool dropped = trim(board);

	/* And the file is written out again where reading it changed anything -
	   duplicates collapsed, unreadable lines dropped, entries trimmed - so that
	   what is on disk is what is held rather than growing by the difference. */
	if (dropped || kept != lines)
		rewrite(board);
}

void ADC::LocalHub::BoardStore::readConfiguration(QuickDC::Preferences* config)
{
	if (!config) return;

	std::vector<BoardState> configured;

	const std::vector<std::string> sections = config->groupNames();

	for (const std::string& section : sections)
	{
		if (!startsWith(section, BOARD_SECTION_PREFIX)) continue;

		const std::string name = section.substr(strlen(BOARD_SECTION_PREFIX));

		if (!ADC::isValidBoardName(name))
		{
			QERR("'%s' is not a board name; ignoring that section", section.c_str());
			continue;
		}

		if (configured.size() >= MAX_BOARDS)
		{
			QERR("More than %zu boards are configured; ignoring '%s'",
				MAX_BOARDS, name.c_str());
			continue;
		}

		BoardState board;
		board.name = name;
		board.title = config->peekString(section, "Title", std::string());
		board.description = config->peekString(section, "Description", std::string());

		board.maxSize = clamped(config->peekNumber(section, "Max post size",
			static_cast<int>(BOARD_DEFAULT_MAX_SIZE)), 1, 64 * 1024 * 1024);

		board.maxEntries = clamped(config->peekNumber(section, "Max entries",
			static_cast<int>(BOARD_DEFAULT_MAX_ENTRIES)), 1, 1000000);

		board.maxEntriesPerAuthor = clamped(config->peekNumber(section,
			"Max entries per author",
			static_cast<int>(BOARD_DEFAULT_MAX_ENTRIES_PER_AUTHOR)), 1, 1000000);

		/* A mask with bits nobody has defined in it would be sent to a client as
		   permissions it cannot read, so what is not a permission is dropped. */
		board.userPermissions = clamped(config->peekNumber(section, "User permissions",
			static_cast<int>(BOARD_DEFAULT_USER_PERMISSIONS)), 0,
			static_cast<int>(BOARD_PERMISSIONS_ALL));

		board.operatorPermissions = clamped(config->peekNumber(section,
			"Operator permissions", static_cast<int>(BOARD_DEFAULT_OPERATOR_PERMISSIONS)),
			0, static_cast<int>(BOARD_PERMISSIONS_ALL));

		board.adminPermissions = clamped(config->peekNumber(section,
			"Admin permissions", static_cast<int>(BOARD_DEFAULT_OPERATOR_PERMISSIONS)),
			0, static_cast<int>(BOARD_PERMISSIONS_ALL));

		board.registeredOnly = config->peekBool(section, "Registered only", false);

		/*
		 * A board that is still configured keeps the index it has already loaded,
		 * rather than reading its file again: the entries in memory are the ones
		 * subscribers have been sent, and a re-read that lost the highest
		 * timestamp handed out would hand it out a second time.
		 */
		const BoardState* held = find(name);
		if (held)
		{
			board.entries = held->entries;
			board.highest = held->highest;
			board.oldest = held->oldest;
		}

		configured.push_back(std::move(board));
	}

	boards = std::move(configured);

	for (BoardState& board : boards)
	{
		if (!board.entries.empty() || board.highest) continue;
		load(board);
	}
}
