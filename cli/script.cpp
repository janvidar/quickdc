/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "script.h"
#include "commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>

namespace {

std::string expand(const QuickDC::Cli::Grammar& grammar, const std::string& text)
{
	std::string out;
	size_t at = 0;

	while (at < text.size())
	{
		const size_t open = text.find("${", at);
		if (open == std::string::npos) break;

		const size_t close = text.find('}', open + 2);
		if (close == std::string::npos) throw QuickDC::Cli::ScriptError("unterminated ${");

		const std::string name = text.substr(open + 2, close - open - 2);
		const auto found = grammar.variables.find(name);
		if (found == grammar.variables.end())
			throw QuickDC::Cli::ScriptError("no variable '" + name + "'; define it with -D " + name + "=...");

		out.append(text, at, open - at);
		out += found->second;
		at = close + 1;
	}

	out.append(text, at, std::string::npos);
	return out;
}

std::vector<std::string> split(const std::string& text)
{
	std::vector<std::string> words;
	size_t at = 0;

	while (true)
	{
		while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
		if (at == text.size()) break;

		std::string word;
		bool quoted = false;

		while (at < text.size() && (quoted || !std::isspace(static_cast<unsigned char>(text[at]))))
		{
			const char c = text[at++];
			if (c == '"')
			{
				quoted = !quoted;
			}
			else if (c == '\\' && quoted && at < text.size())
			{
				word += text[at++];
			}
			else
			{
				word += c;
			}
		}

		if (quoted) throw QuickDC::Cli::ScriptError("unterminated quote");
		words.push_back(std::move(word));
	}

	return words;
}

}

const std::string& QuickDC::Cli::Line::requireCore() const
{
	if (core.empty()) throw ScriptError("there are several cores; address this line with @name");
	return core;
}

std::string QuickDC::Cli::Line::rest(size_t first) const
{
	std::string out;
	for (size_t n = first; n < args.size(); ++n)
	{
		if (n > first) out += ' ';
		out += args[n];
	}
	return out;
}

void QuickDC::Cli::Line::expect(size_t least, size_t most) const
{
	if (args.size() < least || args.size() > most)
		throw ScriptError("wrong number of arguments");
}

bool QuickDC::Cli::parseLine(const Grammar& grammar, int number, const std::string& text, Statement& out)
{
	const size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos || text[first] == '#') return false;

	Line line;
	line.number = number;
	line.source = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);

	std::vector<std::string> words = split(expand(grammar, line.source));

	if (!words.empty() && words.front().size() > 1 && words.front()[0] == '@')
	{
		line.core = words.front().substr(1);
		line.addressed = true;
		words.erase(words.begin());

		if (std::find(grammar.cores.begin(), grammar.cores.end(), line.core) == grammar.cores.end())
			throw ScriptError("there is no core '" + line.core + "'; name it with --core");
	}
	else if (grammar.cores.size() == 1)
	{
		line.core = grammar.cores.front();
	}

	if (words.empty()) throw ScriptError("a core is addressed but no command follows");

	line.verb = words.front();
	line.args.assign(words.begin() + 1, words.end());

	const Verb* verb = findVerb(line.verb);
	if (!verb) throw ScriptError("unknown command '" + line.verb + "'");

	try
	{
		out.step = verb->parse(line);
	}
	catch (const ScriptError& e)
	{
		throw ScriptError(std::string(e.what()) + " (usage: " + verb->usage + ")");
	}

	out.line = std::move(line);
	out.beforeStart = verb->beforeStart;
	return true;
}

std::chrono::milliseconds QuickDC::Cli::parseDuration(const std::string& text)
{
	uint64_t amount = 0;
	const char* begin = text.data();
	const char* end = begin + text.size();
	const auto parsed = std::from_chars(begin, end, amount);
	const std::string unit(parsed.ptr, end);

	if (parsed.ec != std::errc() || parsed.ptr == begin)
		throw ScriptError("'" + text + "' is not a duration");

	if (unit == "ms") return std::chrono::milliseconds(amount);
	if (unit == "s" || unit.empty()) return std::chrono::seconds(amount);
	throw ScriptError("'" + text + "' is not a duration; write it as 500ms or 10s");
}
