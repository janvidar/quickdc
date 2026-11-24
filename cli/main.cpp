/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * qdc: QuickDC without a window. Runs one or more cores, each on a thread of its
 * own, and a script of commands against them; prints every event as a line of
 * JSON; and exits with the verdict.
 */

#include "freeport.h"
#include "runner.h"
#include "script.h"
#include "session.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

const int EXIT_PASSED = 0;
const int EXIT_FAILED = 1;
const int EXIT_USAGE = 2;

void usage(FILE* to)
{
	std::fprintf(to,
		"Usage: qdc [options] <script | ->\n"
		"\n"
		"  --core NAME         run a core called NAME; repeat for several (default: one, 'a')\n"
		"  --home DIR          keep core NAME's files in DIR/NAME (default: a fresh\n"
		"                      temporary directory, removed on exit)\n"
		"  -D NAME=VALUE       define ${NAME} for the script\n"
		"  --free-port NAME    define ${NAME} as a port nothing is listening on\n"
		"  --wait-timeout DUR  how long a wait waits unless it says (default: 10s)\n"
		"  --quiet             print only the events waits matched, and failures\n"
		"  --keep-going        carry on past a failed line; the exit status still fails\n"
		"\n"
		"Exit status: 0 when the script ran to its end, 1 when a line failed,\n"
		"2 for a usage error or a script that does not parse.\n");
}

struct Arguments {
	std::vector<std::string> cores;
	std::string home;
	std::string script;
	QuickDC::Cli::Grammar grammar;
	QuickDC::Cli::Options options;
};

/* Exits on a usage error, since nothing has been started yet. */
Arguments parseArguments(int argc, char** argv)
{
	Arguments out;

	const auto value = [&](int& n) -> std::string {
		if (n + 1 >= argc)
		{
			std::fprintf(stderr, "qdc: %s needs a value\n", argv[n]);
			std::exit(EXIT_USAGE);
		}
		return argv[++n];
	};

	for (int n = 1; n < argc; ++n)
	{
		const std::string arg = argv[n];

		if (arg == "-h" || arg == "--help")
		{
			usage(stdout);
			std::exit(EXIT_PASSED);
		}
		else if (arg == "--core")
		{
			out.cores.push_back(value(n));
		}
		else if (arg == "--home")
		{
			out.home = value(n);
		}
		else if (arg == "-D")
		{
			const std::string definition = value(n);
			const size_t equals = definition.find('=');
			if (equals == std::string::npos || equals == 0)
			{
				std::fprintf(stderr, "qdc: -D wants NAME=VALUE, not '%s'\n", definition.c_str());
				std::exit(EXIT_USAGE);
			}
			out.grammar.variables[definition.substr(0, equals)] = definition.substr(equals + 1);
		}
		else if (arg == "--free-port")
		{
			const std::string name = value(n);
			const int port = QuickDC::Cli::freePort();
			if (port <= 0)
			{
				std::fprintf(stderr, "qdc: no free port for %s: %s\n", name.c_str(), std::strerror(errno));
				std::exit(EXIT_FAILED);
			}
			out.grammar.variables[name] = std::to_string(port);
		}
		else if (arg == "--wait-timeout")
		{
			try
			{
				out.options.waitTimeout = QuickDC::Cli::parseDuration(value(n));
			}
			catch (const QuickDC::Cli::ScriptError& e)
			{
				std::fprintf(stderr, "qdc: --wait-timeout: %s\n", e.what());
				std::exit(EXIT_USAGE);
			}
		}
		else if (arg == "--quiet")
		{
			out.options.quiet = true;
		}
		else if (arg == "--keep-going")
		{
			out.options.keepGoing = true;
		}
		else if (arg.size() > 1 && arg[0] == '-')
		{
			std::fprintf(stderr, "qdc: unknown option '%s'\n", arg.c_str());
			usage(stderr);
			std::exit(EXIT_USAGE);
		}
		else if (out.script.empty())
		{
			out.script = arg;
		}
		else
		{
			std::fprintf(stderr, "qdc: one script at a time\n");
			std::exit(EXIT_USAGE);
		}
	}

	if (out.script.empty())
	{
		usage(stderr);
		std::exit(EXIT_USAGE);
	}

	if (out.cores.empty()) out.cores.push_back("a");

	for (size_t n = 0; n < out.cores.size(); ++n)
	{
		const std::string& name = out.cores[n];
		const bool plain = !name.empty() && name.find_first_of("/@ \t") == std::string::npos
			&& name != "." && name != "..";
		if (!plain)
		{
			std::fprintf(stderr, "qdc: '%s' cannot name a core\n", name.c_str());
			std::exit(EXIT_USAGE);
		}

		for (size_t m = 0; m < n; ++m)
		{
			if (out.cores[m] == name)
			{
				std::fprintf(stderr, "qdc: core '%s' is named twice\n", name.c_str());
				std::exit(EXIT_USAGE);
			}
		}
	}

	out.grammar.cores = out.cores;
	out.options.script = out.script;
	return out;
}

/* The whole script, parsed before anything runs, so a typo costs no cores. */
bool parseScript(const Arguments& arguments, std::vector<QuickDC::Cli::Statement>& statements)
{
	std::ifstream file;
	if (arguments.script != "-")
	{
		file.open(arguments.script);
		if (!file)
		{
			std::fprintf(stderr, "qdc: cannot read '%s'\n", arguments.script.c_str());
			return false;
		}
	}
	std::istream& in = arguments.script == "-" ? std::cin : file;

	bool ok = true;
	bool started = false;
	std::string text;

	for (int number = 1; std::getline(in, text); ++number)
	{
		QuickDC::Cli::Statement statement;

		try
		{
			if (!QuickDC::Cli::parseLine(arguments.grammar, number, text, statement)) continue;

			if (statement.beforeStart && started)
				throw QuickDC::Cli::ScriptError("'" + statement.line.verb
					+ "' is taken before the cores start, so it goes ahead of every other command");

			started = started || !statement.beforeStart;
			statements.push_back(std::move(statement));
		}
		catch (const QuickDC::Cli::ScriptError& e)
		{
			std::fprintf(stderr, "qdc: %s:%d: %s\n", arguments.script.c_str(), number, e.what());
			ok = false;
		}
	}

	return ok;
}

/** A home made for this run, and removed with it. */
class Scratch {
	public:
		~Scratch()
		{
			std::error_code ignored;
			if (!path.empty()) std::filesystem::remove_all(path, ignored);
		}

		bool make()
		{
			const char* tmp = std::getenv("TMPDIR");
			std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/qdc-XXXXXX";
			if (!mkdtemp(pattern.data())) return false;
			path = pattern;
			return true;
		}

		std::string path;
};

int run(const Arguments& arguments, std::vector<QuickDC::Cli::Statement>& statements)
{
	Scratch scratch;
	std::string base = arguments.home;

	if (base.empty())
	{
		if (!scratch.make())
		{
			std::fprintf(stderr, "qdc: cannot make a temporary home: %s\n", std::strerror(errno));
			return EXIT_FAILED;
		}
		base = scratch.path;
	}

	QuickDC::Cli::Runner runner;
	QuickDC::Cli::Session session(runner, arguments.options);
	session.setBase(base);

	int status = EXIT_PASSED;
	size_t next = 0;

	for (; next < statements.size() && statements[next].beforeStart; ++next)
	{
		if (!arguments.options.quiet) session.note("> " + statements[next].line.source);
		if (statements[next].step(session) == QuickDC::Cli::Outcome::Failed) return EXIT_FAILED;
	}

	for (const std::string& name : arguments.cores)
	{
		const std::string home = base + "/" + name;

		std::error_code error;
		std::filesystem::create_directories(home, error);
		if (error)
		{
			std::fprintf(stderr, "qdc: cannot make '%s': %s\n", home.c_str(), error.message().c_str());
			return EXIT_FAILED;
		}

		std::string failure;
		if (!runner.start(name, home, session.presets(name), failure))
		{
			std::fprintf(stderr, "qdc: core '%s' did not start: %s\n", name.c_str(), failure.c_str());
			return EXIT_FAILED;
		}

		if (!arguments.options.quiet) session.note("qdc: core '" + name + "' in " + home);
	}

	for (; next < statements.size(); ++next)
	{
		const QuickDC::Cli::Statement& statement = statements[next];
		if (!arguments.options.quiet) session.note("> " + statement.line.source);

		const QuickDC::Cli::Outcome outcome = statement.step(session);
		if (outcome == QuickDC::Cli::Outcome::Quit) break;
		if (outcome == QuickDC::Cli::Outcome::Failed)
		{
			status = EXIT_FAILED;
			if (!arguments.options.keepGoing) break;
		}
	}

	runner.stop();

	/* What the cores said on their way out, for anybody reading along. */
	session.pump(std::chrono::steady_clock::now());

	return status;
}

}

int main(int argc, char** argv)
{
	const Arguments arguments = parseArguments(argc, argv);

	std::vector<QuickDC::Cli::Statement> statements;
	if (!parseScript(arguments, statements)) return EXIT_USAGE;

	return run(arguments, statements);
}
