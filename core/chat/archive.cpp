/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "chat/archive.h"

#include <samurai/io/file.h>

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

/** How much of the file to take at a time when walking back from its end. */
constexpr size_t CHUNK = 64 * 1024;

/**
 * How far back a replayed line is recognised.
 *
 * A hub replays a buffer, not its whole history, and this is well past what any
 * of them keep. A line older than the last few thousand would be written again
 * rather than recognised, which is a duplicate in the record rather than
 * anything worse.
 */
constexpr size_t REMEMBERED = 4000;

/**
 * How much of a name survives into the file name, per half.
 *
 * A nickname is up to sixty-three arbitrary bytes on NMDC and every one of them
 * may need escaping, so the readable part is cut rather than trusted to fit. The
 * digest is what keeps two conversations apart; this is only so that somebody
 * looking in the directory can tell what they are looking at.
 */
constexpr size_t READABLE = 32;

/**
 * A 64 bit FNV-1a of the key, in hex.
 *
 * Enough to separate the conversations one client holds, which is thousands at
 * the very most. Nothing is decided by this value - a collision would merge two
 * transcripts, not admit anybody anywhere - so it does not need to be a
 * cryptographic hash, and this way the archive does not drag one in.
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

/* Letters and digits survive; a run of anything else becomes one '-'. */
std::string readable_part(const std::string& text)
{
	std::string out;

	for (const char c : text)
	{
		const unsigned char u = static_cast<unsigned char>(c);

		if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9'))
			out += static_cast<char>(u);
		else if (u >= 'A' && u <= 'Z')
			out += static_cast<char>(u - 'A' + 'a');
		else if (!out.empty() && out.back() != '-')
			out += '-';

		if (out.size() >= READABLE) break;
	}

	while (!out.empty() && out.back() == '-') out.pop_back();

	return out;
}

/*
 * Escaped the way ADC escapes what it sends, because a transcript is a line
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

/** The field named by 'tag', or the empty string when the line has none. */
std::string field(const std::vector<std::string>& parts, const char* tag)
{
	for (const std::string& part : parts)
	{
		if (part.size() >= 2 && part[0] == tag[0] && part[1] == tag[1])
			return unescape(part.substr(2));
	}

	return std::string();
}

/**
 * Everything that identifies a line, joined into one string.
 *
 * The stamp, who said it and what they said. Two lines agreeing on all three
 * are the same line said once - a hub replaying its history sends exactly that,
 * and a person saying the same words twice within the same second is the case
 * this cannot tell apart from it. Losing the second of those costs a repeated
 * line; keeping it would show the whole of a replayed history twice over.
 */
std::string key_of(const QuickDC::ChatArchive::Line& line)
{
	return std::to_string(line.time) + '\n' + line.source + '\n'
		+ line.nick + '\n' + line.hub + '\n' + line.text;
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

}

QuickDC::ChatArchive::ChatArchive(const std::string& directory_)
	: directory(directory_)
	, usable(false)
{
	if (directory.empty()) return;

	/*
	 * mkdir() rather than a walk that makes each component: everything above
	 * this is made by Core as it starts, so the one level is all that is left -
	 * and a directory that is already there is not an error worth acting on.
	 */
	Samurai::IO::File::mkdir(directory.c_str(), 0700);

	usable = Samurai::IO::File(directory).isDirectory();
}

bool QuickDC::ChatArchive::isUsable() const
{
	return usable;
}

std::string QuickDC::ChatArchive::pathFor(const std::string& hubUrl,
                                          const std::string& conversation) const
{
	/*
	 * Joined with a byte that cannot appear in either before it is hashed, so a
	 * hub whose address runs into a nickname cannot land on the file of another
	 * pairing that happens to read the same way end to end.
	 */
	std::string name = readable_part(hubUrl);

	name += conversation.empty() ? "-chat-" : "-" + readable_part(conversation) + "-";
	name += digest_of(hubUrl + '\n' + conversation);

	return directory + "/" + name + ".log";
}

void QuickDC::ChatArchive::remember(const std::string& path,
                                   const std::string& hubUrl,
                                   const std::string& conversation) const
{
	if (seen.count(path)) return;

	std::set<std::string>& keys = seen[path];
	uint64_t high = 0;

	for (const Line& line : tail(hubUrl, conversation, REMEMBERED))
	{
		keys.insert(key_of(line));
		if (line.time > high) high = line.time;
	}

	newest[path] = high;
}

bool QuickDC::ChatArchive::append(const std::string& hubUrl,
                                  const std::string& conversation,
                                  const Line& line)
{
	/*
	 * Nothing is kept, so nothing can be a repeat of anything - and a caller
	 * deciding whether to show a message must not be told to drop it.
	 */
	if (!usable) return true;

	const std::string path = pathFor(hubUrl, conversation);

	remember(path, hubUrl, conversation);

	/*
	 * Only a line that is not newer than everything on file can be a repeat of
	 * any of it, which is what keeps live chat off the comparison entirely: it
	 * arrives stamped now, and now is newer than anything already written.
	 */
	if (line.time <= newest[path] && seen[path].count(key_of(line))) return false;

	std::string out = "T" + std::to_string(line.time);

	if (!line.nick.empty())   out += " NI" + escape(line.nick);
	if (!line.source.empty()) out += " SI" + escape(line.source);
	if (!line.hub.empty())    out += " HB" + escape(line.hub);

	std::string flags;
	if (line.op) flags += 'O';
	if (line.action) flags += 'A';
	if (line.richText) flags += 'R';
	if (line.privateMessage) flags += 'P';

	if (!flags.empty()) out += " FL" + flags;

	/* Last, though the reader does not care: it is the one that is always there. */
	out += " MS" + escape(line.text);
	out += "\n";

	Samurai::IO::File file(pathFor(hubUrl, conversation));

	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Append))
		return false;

	file.write(out.data(), out.size());
	file.close();

	seen[path].insert(key_of(line));
	if (line.time > newest[path]) newest[path] = line.time;

	return true;
}

bool QuickDC::ChatArchive::contains(const std::string& hubUrl,
                                    const std::string& conversation) const
{
	return usable && Samurai::IO::File::exists(pathFor(hubUrl, conversation).c_str());
}

void QuickDC::ChatArchive::forget(const std::string& hubUrl,
                                  const std::string& conversation)
{
	if (!usable) return;

	const std::string path = pathFor(hubUrl, conversation);

	Samurai::IO::File(path).remove();

	/* Or the lines just thrown away would go on being recognised as repeats. */
	seen.erase(path);
	newest.erase(path);
}

std::vector<QuickDC::ChatArchive::Line>
QuickDC::ChatArchive::tail(const std::string& hubUrl,
                           const std::string& conversation,
                           size_t limit) const
{
	std::vector<Line> lines;
	if (!usable || !limit) return lines;

	Samurai::IO::File file(pathFor(hubUrl, conversation));
	if (!file.open(Samurai::IO::File::Mode::Read)) return lines;

	/*
	 * A chunk at a time from the end, keeping whatever did not begin with a
	 * newline to join to the chunk before it - a record that straddles the seam
	 * would otherwise be read as two halves, neither of which parses.
	 */
	std::vector<std::string> found;
	std::string carry;
	off_t at = file.size();

	while (at > 0 && found.size() < limit)
	{
		const size_t want = std::min<size_t>(CHUNK, static_cast<size_t>(at));
		at -= static_cast<off_t>(want);

		if (!file.seek(at)) break;

		std::string chunk(want, '\0');
		const ssize_t got = file.read(chunk.data(), want);
		if (got <= 0) break;

		chunk.resize(static_cast<size_t>(got));
		chunk += carry;
		carry.clear();

		size_t from = 0;

		/*
		 * Everything before the first newline may be the tail of a record that
		 * began in the chunk before this one, so it is carried rather than read -
		 * unless there is no chunk before it, when it is a whole record.
		 */
		if (at > 0)
		{
			const size_t first = chunk.find('\n');

			if (first == std::string::npos)
			{
				carry = chunk;
				continue;
			}

			carry = chunk.substr(0, first);
			from = first + 1;
		}

		/* Backwards, because the recent end is what is being gathered. */
		size_t end = chunk.size();

		while (end > from && found.size() < limit)
		{
			const size_t start = chunk.rfind('\n', end - 1);
			const size_t at_ = (start == std::string::npos || start < from)
				? from : start + 1;

			if (end > at_) found.push_back(chunk.substr(at_, end - at_));

			if (at_ == from) break;
			end = at_ - 1;
		}
	}

	file.close();

	/*
	 * Gathered newest first, and a conversation reads the other way. A record
	 * that does not carry a time is dropped: the last one may have been half
	 * written when the process went, and one unreadable line is no reason to
	 * lose the rest of somebody's history.
	 */
	lines.reserve(found.size());

	for (size_t n = found.size(); n > 0; n--)
	{
		const std::string& raw = found[n - 1];
		if (raw.empty()) continue;

		const std::vector<std::string> parts = split(raw);

		/* The stamp leads every record, and a record without one is not one. */
		if (parts.empty() || parts[0].size() < 2 || parts[0][0] != 'T') continue;

		Line line;
		line.time = strtoull(parts[0].c_str() + 1, 0, 10);
		line.nick = field(parts, "NI");
		line.source = field(parts, "SI");
		line.hub = field(parts, "HB");
		line.text = field(parts, "MS");

		const std::string flags = field(parts, "FL");
		line.op = flags.find('O') != std::string::npos;
		line.action = flags.find('A') != std::string::npos;
		line.richText = flags.find('R') != std::string::npos;
		line.privateMessage = flags.find('P') != std::string::npos;

		lines.push_back(line);
	}

	/*
	 * By the time each line was said, not the order they were written. They
	 * almost always agree - a replay is filled in before the session says
	 * anything of its own - but a hub that hands over a line older than one
	 * already on file would otherwise have it drawn at the bottom.
	 *
	 * Stable, so that lines sharing a stamp keep the order they arrived in.
	 */
	std::stable_sort(lines.begin(), lines.end(),
		[](const Line& a, const Line& b) { return a.time < b.time; });

	return lines;
}
