/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "chat/chatmedia.h"

#include "api/huburl.h"
#include "config/preferences.h"
#include "config/settings.h"

#include <samurai/stdc.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <system_error>

namespace {

constexpr size_t TTH_LENGTH = 39;
constexpr std::string_view TIGER = "urn:tree:tiger:";
constexpr std::string_view PASTED = "pasted-";

int hexValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

std::string percentDecoded(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t n = 0; n < text.size(); ++n)
	{
		if (text[n] == '%' && n + 2 < text.size())
		{
			const int high = hexValue(text[n + 1]);
			const int low = hexValue(text[n + 2]);
			if (high >= 0 && low >= 0)
			{
				out += static_cast<char>(high * 16 + low);
				n += 2;
				continue;
			}
		}
		out += text[n];
	}
	return out;
}

std::string_view trimmed(std::string_view text)
{
	const size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos) return std::string_view();
	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool isBase32Root(const std::string& tth)
{
	return tth.size() == TTH_LENGTH && std::all_of(tth.begin(), tth.end(), [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
	});
}

bool isPunctuation(char c)
{
	return std::ispunct(static_cast<unsigned char>(c)) != 0;
}

/* The end of the code span whose opening backticks start at 'at', or npos where they open none. */
size_t codeSpanEnd(std::string_view text, size_t at, size_t& run)
{
	run = 0;
	while (at + run < text.size() && text[at + run] == '`') ++run;

	for (size_t n = at + run; n < text.size();)
	{
		if (text[n] != '`') { ++n; continue; }
		size_t closing = 0;
		while (n + closing < text.size() && text[n + closing] == '`') ++closing;
		if (closing == run) return n + closing;
		n += closing;
	}
	return std::string_view::npos;
}

/* The ']' closing the '[' at 'open', where the text between is link text; npos where none does. */
size_t closingBracket(std::string_view text, size_t open)
{
	int depth = 0;
	for (size_t n = open; n < text.size(); ++n)
	{
		const char c = text[n];
		if (c == '\\' && n + 1 < text.size() && isPunctuation(text[n + 1])) { ++n; continue; }
		if (c == '\n' && n + 1 < text.size() && text[n + 1] == '\n') return std::string_view::npos;
		if (c == '`')
		{
			size_t run = 0;
			const size_t end = codeSpanEnd(text, n, run);
			n = (end == std::string_view::npos ? n + run : end) - 1;
			continue;
		}
		if (c == '[') ++depth;
		else if (c == ']' && --depth == 0) return n;
	}
	return std::string_view::npos;
}

void skipSpace(std::string_view text, size_t& at)
{
	bool newline = false;
	while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || (text[at] == '\n' && !newline)))
	{
		if (text[at] == '\n') newline = true;
		++at;
	}
}

std::string unescaped(std::string_view text)
{
	std::string out;
	for (size_t n = 0; n < text.size(); ++n)
	{
		if (text[n] == '\\' && n + 1 < text.size() && isPunctuation(text[n + 1])) ++n;
		out += text[n];
	}
	return out;
}

/*
 * The destination of '(dest "title")' starting at 'at', just past the '('; 'at'
 * ends past the ')'. False where what follows is not a link's destination.
 */
bool destination(std::string_view text, size_t& at, std::string& dest)
{
	skipSpace(text, at);
	if (at >= text.size()) return false;

	if (text[at] == '<')
	{
		const size_t end = text.find_first_of(">\n<", at + 1);
		if (end == std::string_view::npos || text[end] != '>') return false;
		dest = unescaped(text.substr(at + 1, end - at - 1));
		at = end + 1;
	}
	else
	{
		const size_t start = at;
		int parens = 0;
		while (at < text.size())
		{
			const char c = text[at];
			if (c == '\\' && at + 1 < text.size() && isPunctuation(text[at + 1])) { at += 2; continue; }
			if (static_cast<unsigned char>(c) <= ' ') break;
			if (c == '(') ++parens;
			if (c == ')' && parens-- == 0) break;
			++at;
		}
		if (parens > 0) return false;
		dest = unescaped(text.substr(start, at - start));
	}

	skipSpace(text, at);
	if (at < text.size() && (text[at] == '"' || text[at] == '\'' || text[at] == '('))
	{
		const char close = text[at] == '(' ? ')' : text[at];
		size_t n = at + 1;
		while (n < text.size() && text[n] != close)
		{
			if (text[n] == '\\' && n + 1 < text.size()) ++n;
			++n;
		}
		if (n >= text.size()) return false;
		at = n + 1;
		skipSpace(text, at);
	}

	if (at >= text.size() || text[at] != ')') return false;
	++at;
	return true;
}

void scanInline(std::string_view text, std::vector<QuickDC::ChatAttachment>& found)
{
	size_t at = 0;
	while (at < text.size())
	{
		const char c = text[at];

		if (c == '\\' && at + 1 < text.size() && isPunctuation(text[at + 1]))
		{
			at += 2;
			continue;
		}

		if (c == '`')
		{
			size_t run = 0;
			const size_t end = codeSpanEnd(text, at, run);
			at = end == std::string_view::npos ? at + run : end;
			continue;
		}

		if (c == '<')
		{
			const size_t end = text.find_first_of("> \t\n<", at + 1);
			if (end != std::string_view::npos && text[end] == '>')
			{
				const std::string_view inside = text.substr(at + 1, end - at - 1);
				if (Samurai::Util::istarts_with(inside, "magnet:"))
				{
					QuickDC::ChatAttachment file = QuickDC::readMagnet(inside);
					if (!file.tth.empty()) found.push_back(std::move(file));
					at = end + 1;
					continue;
				}
			}
			++at;
			continue;
		}

		const bool image = c == '!' && at + 1 < text.size() && text[at + 1] == '[';
		if (c != '[' && !image)
		{
			++at;
			continue;
		}

		const size_t open = image ? at + 1 : at;
		const size_t close = closingBracket(text, open);
		size_t after = close == std::string_view::npos ? close : close + 1;
		std::string dest;
		if (close == std::string_view::npos || after >= text.size() || text[after] != '('
		    || !destination(text, ++after, dest))
		{
			at = open + 1;
			continue;
		}

		std::vector<QuickDC::ChatAttachment> inner;
		scanInline(text.substr(open + 1, close - open - 1), inner);

		QuickDC::ChatAttachment file = QuickDC::readMagnet(dest);
		if (!file.tth.empty())
		{
			file.inlineMedia = image;
			found.push_back(std::move(file));
		}
		for (QuickDC::ChatAttachment& one : inner) found.push_back(std::move(one));

		at = after;
	}
}

bool isBlank(std::string_view line)
{
	return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

/* The fence a line opens or closes, as its character and length; 0 where it is none. */
size_t fence(std::string_view line, char& mark)
{
	size_t indent = 0;
	while (indent < line.size() && indent < 4 && line[indent] == ' ') ++indent;
	if (indent > 3 || indent >= line.size()) return 0;
	const char c = line[indent];
	if (c != '`' && c != '~') return 0;
	size_t run = 0;
	while (indent + run < line.size() && line[indent + run] == c) ++run;
	if (run < 3) return 0;
	mark = c;
	return run;
}

bool indentedCode(std::string_view line)
{
	return (line.size() >= 4 && line.substr(0, 4) == "    ") || (!line.empty() && line[0] == '\t');
}

}

QuickDC::ChatAttachment QuickDC::readMagnet(std::string_view uri)
{
	ChatAttachment file;

	const size_t colon = uri.find(':');
	if (colon == std::string_view::npos || !Samurai::Util::iequals(uri.substr(0, colon), "magnet"))
		return file;

	const size_t question = uri.find('?', colon);
	if (question == std::string_view::npos) return file;

	std::string_view query = uri.substr(question + 1);
	if (const size_t hash = query.find('#'); hash != std::string_view::npos) query = query.substr(0, hash);

	while (!query.empty())
	{
		const size_t amp = query.find('&');
		const std::string_view item = query.substr(0, amp);
		query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);

		const size_t equals = item.find('=');
		const std::string key = percentDecoded(item.substr(0, equals));
		const std::string value = equals == std::string_view::npos ? std::string()
		                                                           : percentDecoded(item.substr(equals + 1));

		if (Samurai::Util::iequals(key, "xt"))
		{
			if (Samurai::Util::istarts_with(value, TIGER))
			{
				std::string tth(trimmed(std::string_view(value).substr(TIGER.size())));
				for (char& c : tth) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
				file.tth = std::move(tth);
			}
		}
		else if (Samurai::Util::iequals(key, "dn"))
		{
			const size_t slash = value.find_last_of("/\\");
			file.name = std::string(trimmed(slash == std::string::npos ? std::string_view(value)
			                                                           : std::string_view(value).substr(slash + 1)));
			if (file.name == "." || file.name == "..") file.name.clear();
		}
		else if (Samurai::Util::iequals(key, "xl"))
		{
			if (!value.empty() && value.size() < 20
			    && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
				file.size = std::stoull(value);
		}
	}

	if (!isBase32Root(file.tth)) file.tth.clear();
	if (file.name.empty()) file.name = file.tth;
	return file;
}

std::vector<QuickDC::ChatAttachment> QuickDC::attachmentsIn(std::string_view markdown)
{
	std::vector<ChatAttachment> found;
	std::string paragraph;

	const auto flush = [&] {
		scanInline(paragraph, found);
		paragraph.clear();
	};

	char mark = 0;
	size_t fenced = 0;
	bool previousBlank = true;
	bool inCode = false;

	size_t at = 0;
	while (at <= markdown.size())
	{
		const size_t end = std::min(markdown.find('\n', at), markdown.size());
		std::string_view line = markdown.substr(at, end - at);
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		at = end + 1;

		char opened = 0;
		if (fenced)
		{
			const size_t run = fence(line, opened);
			if (run >= fenced && opened == mark && isBlank(line.substr(line.find(mark) + run)))
				fenced = 0;
			previousBlank = false;
			continue;
		}

		if (const size_t run = fence(line, opened))
		{
			flush();
			fenced = run;
			mark = opened;
			previousBlank = false;
			continue;
		}

		if (isBlank(line))
		{
			flush();
			previousBlank = true;
			inCode = false;
			continue;
		}

		if (indentedCode(line) && (previousBlank || inCode))
		{
			flush();
			inCode = true;
			previousBlank = false;
			continue;
		}

		inCode = false;
		previousBlank = false;
		if (!paragraph.empty()) paragraph += '\n';
		paragraph.append(line);
	}

	flush();
	return found;
}

QuickDC::ChatMediaSkip QuickDC::chatMediaVerdict(uint64_t size, bool fetching, uint64_t limit)
{
	if (!fetching) return ChatMediaSkip::Off;
	if (size == 0) return ChatMediaSkip::Unsized;
	if (limit > 0 && size > limit) return ChatMediaSkip::TooLarge;
	return ChatMediaSkip::None;
}

bool QuickDC::chatMediaFetchedOn(const Preferences& config, const Preferences& session, const std::string& hubUrl)
{
	const bool general = Settings::read(config, Settings::ChatMediaFetch);
	if (hubUrl.empty()) return general;
	return session.peekBool(hubSettingsGroup(hubUrl), Settings::ChatMediaFetch.key, general);
}

std::string QuickDC::pastedName(std::time_t when, std::string_view suffix)
{
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &when);
#else
	localtime_r(&when, &local);
#endif
	char stamp[32];
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

	std::string clean;
	for (char c : suffix)
		if (std::isalnum(static_cast<unsigned char>(c)) && clean.size() < 8)
			clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (clean.empty()) clean = "png";

	return std::string(PASTED) + stamp + "." + clean;
}

int QuickDC::sweepPasted(const std::string& folder, int days, std::time_t now)
{
	if (days <= 0 || folder.empty()) return 0;

	namespace fs = std::filesystem;
	std::error_code error;
	fs::directory_iterator entries(folder, error);
	if (error) return 0;

	const auto cutoff = std::chrono::system_clock::from_time_t(now) - std::chrono::hours(24) * days;

	int removed = 0;
	for (const fs::directory_entry& entry : entries)
	{
		if (!entry.is_regular_file(error)) continue;
		if (entry.path().filename().string().rfind(PASTED, 0) != 0) continue;

		const auto written = entry.last_write_time(error);
		if (error) continue;
#ifdef _MSC_VER
		const auto when = std::chrono::clock_cast<std::chrono::system_clock>(written);
#else
		const auto when = std::chrono::file_clock::to_sys(written);
#endif
		if (when >= cutoff) continue;

		if (fs::remove(entry.path(), error)) ++removed;
	}
	return removed;
}
