/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_SCRIPT_H
#define HAVE_QUICKDC_CLI_SCRIPT_H

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace QuickDC::Cli {

class Session;

/** A line that cannot be run as written; what() says why, without the line number. */
class ScriptError : public std::runtime_error {
	public:
		using std::runtime_error::runtime_error;
};

/** One command line, split into words; double quotes keep a word's spaces. */
struct Line {
	int number = 0;
	std::string source;

	/** The core named by an '@name' prefix, or the only one; empty for none and several. */
	std::string core;
	bool addressed = false;

	std::string verb;
	std::vector<std::string> args;

	/** The core this line acts on, or a ScriptError where that is ambiguous. */
	const std::string& requireCore() const;

	/** Arguments from 'first' on, joined by single spaces. */
	std::string rest(size_t first) const;

	/** Fails unless there are between 'least' and 'most' arguments. */
	void expect(size_t least, size_t most) const;
};

/** What running a step leaves the script to do. */
enum class Outcome { Next, Failed, Quit };

/** A parsed command, ready to run against a session. */
using Step = std::function<Outcome(Session&)>;

struct Statement {
	Line line;
	Step step;
	bool beforeStart = false;
	bool anytime = false;
};

struct Grammar {
	std::vector<std::string> cores;
	std::map<std::string, std::string> variables;
};

/**
 * The statement on one line of source, or nothing for a blank line or a
 * comment. Throws ScriptError for anything that cannot run.
 */
bool parseLine(const Grammar& grammar, int number, const std::string& text, Statement& out);

/** "250ms", "10s", or a bare number of seconds. */
std::chrono::milliseconds parseDuration(const std::string& text);

}

#endif // HAVE_QUICKDC_CLI_SCRIPT_H
