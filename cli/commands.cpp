/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "commands.h"
#include "eventjson.h"
#include "session.h"

#include <algorithm>
#include <limits>

namespace Api = QuickDC::Api;
namespace Cmd = QuickDC::Api::Commands;

namespace {

using QuickDC::Cli::Line;
using QuickDC::Cli::Outcome;
using QuickDC::Cli::ScriptError;
using QuickDC::Cli::Session;
using QuickDC::Cli::Step;

Api::SettingsStore store(const std::string& word)
{
	if (word == "application") return Api::SettingsStore::Application;
	if (word == "session") return Api::SettingsStore::Session;
	throw ScriptError("'" + word + "' is not a store; it is application or session");
}

/** A command about the hub the script named 'hub', which is looked up as it runs. */
template<typename Make>
Step onHub(const Line& line, const std::string& hub, Make make)
{
	const std::string core = line.requireCore();

	return [line, core, hub, make](Session& session) {
		const std::optional<Api::HubHandle> handle = session.hub(core, hub);
		if (!handle) return session.fail(line, "core '" + core + "' has no hub '" + hub + "' open");
		return session.submit(line, core, make(session, *handle));
	};
}

Step parseSet(const Line& line)
{
	line.expect(4, std::numeric_limits<size_t>::max());

	Cmd::SetSetting command;
	command.store = store(line.args[0]);
	command.group = line.args[1];
	command.key = line.args[2];
	command.value = line.rest(3);
	const std::string core = line.requireCore();

	return [line, core, command](Session& session) mutable {
		command.origin = session.nextRequest();
		return session.submit(line, core, command);
	};
}

Step parseUnset(const Line& line)
{
	line.expect(3, 3);

	Cmd::SetSetting command;
	command.store = store(line.args[0]);
	command.group = line.args[1];
	command.key = line.args[2];
	command.remove = true;
	const std::string core = line.requireCore();

	return [line, core, command](Session& session) mutable {
		command.origin = session.nextRequest();
		return session.submit(line, core, command);
	};
}

/* Unaddressed with several cores, it is for all of them. */
Step parsePreset(const Line& line)
{
	line.expect(4, std::numeric_limits<size_t>::max());

	const Api::SettingsStore where = store(line.args[0]);
	const std::string group = line.args[1];
	const std::string key = line.args[2];
	const std::string value = line.rest(3);
	const std::string core = line.core;

	return [core, where, group, key, value](Session& session) {
		session.preset(core, where, group, key, value);
		return Outcome::Next;
	};
}

Step parseSave(const Line& line)
{
	line.expect(0, 1);

	Cmd::SaveSettings command;
	command.store = line.args.empty() ? Api::SettingsStore::Application : store(line.args[0]);
	const std::string core = line.requireCore();

	return [line, core, command](Session& session) mutable {
		command.request = session.nextRequest();
		return session.submit(line, core, command);
	};
}

Step parseOpen(const Line& line)
{
	if (line.args.size() != 1 && !(line.args.size() == 3 && line.args[1] == "as"))
		throw ScriptError("wrong arguments");

	const std::string url = line.args[0];
	const std::string name = line.args.size() == 3 ? line.args[2] : url;
	const std::string core = line.requireCore();

	return [line, core, url, name](Session& session) {
		if (session.hub(core, name))
			return session.fail(line, "core '" + core + "' already has a hub called '" + name + "'");

		return session.submit(line, core, Cmd::OpenHub{session.openHub(core, name), url});
	};
}

Step parseClose(const Line& line)
{
	line.expect(1, 1);
	const std::string core = line.requireCore();
	const std::string name = line.args[0];
	const Step close = onHub(line, name, [](Session&, Api::HubHandle hub) {
		return Api::Command(Cmd::CloseHub{hub});
	});

	return [close, core, name](Session& session) {
		const Outcome outcome = close(session);
		if (outcome == Outcome::Next) session.forgetHub(core, name);
		return outcome;
	};
}

Step parseConnect(const Line& line)
{
	line.expect(1, 1);
	return onHub(line, line.args[0], [](Session&, Api::HubHandle hub) {
		return Api::Command(Cmd::ConnectHub{hub});
	});
}

Step parseDisconnect(const Line& line)
{
	line.expect(1, 1);
	return onHub(line, line.args[0], [](Session&, Api::HubHandle hub) {
		return Api::Command(Cmd::DisconnectHub{hub});
	});
}

Step parseChat(const Line& line)
{
	line.expect(2, std::numeric_limits<size_t>::max());
	const std::string text = line.rest(1);
	return onHub(line, line.args[0], [text](Session&, Api::HubHandle hub) {
		return Api::Command(Cmd::SendChat{hub, text, false});
	});
}

/* A nickname this core has seen on the hub, or else taken to be the user's id. */
Step parsePrivate(const Line& line)
{
	line.expect(3, std::numeric_limits<size_t>::max());
	const std::string who = line.args[1];
	const std::string text = line.rest(2);
	const std::string core = line.requireCore();

	return onHub(line, line.args[0], [core, who, text](Session& session, Api::HubHandle hub) {
		const std::string id = session.userId(core, hub, who).value_or(who);
		return Api::Command(Cmd::SendPrivateChat{hub, id, text, false});
	});
}

Step parsePassword(const Line& line)
{
	line.expect(2, 2);
	const std::string password = line.args[1];
	return onHub(line, line.args[0], [password](Session&, Api::HubHandle hub) {
		return Api::Command(Cmd::SendPassword{hub, password});
	});
}

Step parseWait(const Line& line)
{
	line.expect(1, std::numeric_limits<size_t>::max());

	const std::vector<std::string>& names = QuickDC::Cli::eventNames();
	if (std::find(names.begin(), names.end(), line.args[0]) == names.end())
		throw ScriptError("there is no event '" + line.args[0] + "'");

	QuickDC::Cli::Expectation expected;
	expected.core = line.addressed ? line.core : std::string();
	expected.event = line.args[0];
	std::optional<std::chrono::milliseconds> timeout;

	for (size_t n = 1; n < line.args.size(); ++n)
	{
		const std::string& arg = line.args[n];
		const size_t equals = arg.find('=');
		if (equals == std::string::npos || equals == 0)
			throw ScriptError("'" + arg + "' is not field=value");

		const std::string field = arg.substr(0, equals);
		const std::string value = arg.substr(equals + 1);

		if (field == "timeout") timeout = QuickDC::Cli::parseDuration(value);
		else expected.fields.emplace_back(field, value);
	}

	return [line, expected, timeout](Session& session) mutable {
		expected.timeout = timeout.value_or(session.options().waitTimeout);
		return session.wait(line, expected);
	};
}

Step parseSleep(const Line& line)
{
	line.expect(1, 1);
	const std::chrono::milliseconds duration = QuickDC::Cli::parseDuration(line.args[0]);

	return [duration](Session& session) {
		session.pump(std::chrono::steady_clock::now() + duration);
		return Outcome::Next;
	};
}

Step parseEcho(const Line& line)
{
	const std::string text = line.rest(0);
	return [text](Session& session) {
		session.note(text);
		return Outcome::Next;
	};
}

Step parseQuit(const Line& line)
{
	line.expect(0, 0);
	return [](Session&) { return Outcome::Quit; };
}

}

const std::vector<QuickDC::Cli::Verb>& QuickDC::Cli::verbs()
{
	static const std::vector<Verb> table = {
		{"preset",     "preset <application|session> <group> <key> <value...>", parsePreset, true},
		{"set",        "set <application|session> <group> <key> <value...>", parseSet},
		{"unset",      "unset <application|session> <group> <key>",          parseUnset},
		{"save",       "save [application|session]",                         parseSave},
		{"open",       "open <url> [as <name>]",                             parseOpen},
		{"close",      "close <hub>",                                        parseClose},
		{"connect",    "connect <hub>",                                      parseConnect},
		{"disconnect", "disconnect <hub>",                                   parseDisconnect},
		{"chat",       "chat <hub> <text...>",                               parseChat},
		{"pm",         "pm <hub> <nick|id> <text...>",                       parsePrivate},
		{"password",   "password <hub> <password>",                          parsePassword},
		{"wait",       "wait <Event> [field=value ...] [timeout=<N>s|<N>ms]", parseWait},
		{"sleep",      "sleep <N>s|<N>ms",                                   parseSleep},
		{"echo",       "echo <text...>",                                     parseEcho},
		{"quit",       "quit",                                               parseQuit},
	};
	return table;
}

const QuickDC::Cli::Verb* QuickDC::Cli::findVerb(const std::string& name)
{
	for (const Verb& verb : verbs())
		if (name == verb.name) return &verb;
	return nullptr;
}
