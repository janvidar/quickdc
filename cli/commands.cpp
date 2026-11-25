/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "commands.h"
#include "eventjson.h"
#include "session.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>

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

QuickDC::NetworkMode networkMode(const std::string& word)
{
	if (word == "automatic") return QuickDC::NetworkMode::Automatic;
	if (word == "advanced") return QuickDC::NetworkMode::Advanced;
	if (word == "passive") return QuickDC::NetworkMode::Passive;
	throw ScriptError("'" + word + "' is not a mode; it is automatic, advanced or passive");
}

/* What advanced leaves to the user, named; the other modes decide it. */
Step parseMode(const Line& line)
{
	line.expect(1, 3);

	Cmd::SetConnectionMode command;
	command.mode = networkMode(line.args[0]);
	for (size_t n = 1; n < line.args.size(); ++n)
	{
		if (line.args[n] == "upnp") command.useUpnp = true;
		else if (line.args[n] == "force-active") command.forceActive = true;
		else throw ScriptError("'" + line.args[n] + "' is neither upnp nor force-active");
	}
	const std::string core = line.requireCore();

	return [line, core, command](Session& session) mutable {
		command.origin = session.nextRequest();
		return session.submit(line, core, command);
	};
}

Step parseState(const Line& line)
{
	line.expect(0, 0);
	const std::string core = line.requireCore();

	return [line, core](Session& session) {
		return session.submit(line, core, Cmd::ReportState{session.nextRequest()});
	};
}

/* A command that names one thing, given whole as the rest of the line. */
template<typename Command>
Step parseNaming(const Line& line)
{
	line.expect(1, std::numeric_limits<size_t>::max());
	const std::string what = line.rest(0);
	const std::string core = line.requireCore();

	return [line, core, what](Session& session) { return session.submit(line, core, Command{what}); };
}

template<typename Command>
Step parseBare(const Line& line)
{
	line.expect(0, 0);
	const std::string core = line.requireCore();

	return [line, core](Session& session) { return session.submit(line, core, Command{}); };
}

/* A path is the core's own, relative to its home as everything else here is. */
Step parseCompletedForget(const Line& line)
{
	line.expect(1, 1);
	const std::string path = line.args[0];
	const std::string core = line.requireCore();

	return [line, core, path](Session& session) {
		return session.submit(line, core, Cmd::ForgetCompletedDownload{session.resolve(line, path)});
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

		return session.submit(line, core, Cmd::OpenHub{session.openHub(core, name, url), url});
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

/* Markdown, with '$NAME' standing for what was remembered as NAME. */
Step parseChatMarkdown(const Line& line)
{
	line.expect(2, std::numeric_limits<size_t>::max());
	const std::string text = line.rest(1);
	return onHub(line, line.args[0], [text](Session& session, Api::HubHandle hub) {
		return Api::Command(Cmd::SendChat{hub, session.expand(text), true});
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
		const std::optional<Api::User> user = session.user(core, hub, who);
		const std::string id = user ? user->id : who;
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

uint64_t parseSize(const std::string& text)
{
	uint64_t amount = 0;
	const char* begin = text.data();
	const char* end = begin + text.size();
	const auto parsed = std::from_chars(begin, end, amount);
	const std::string unit(parsed.ptr, end);

	if (parsed.ec != std::errc() || parsed.ptr == begin)
		throw ScriptError("'" + text + "' is not a size");

	if (unit.empty()) return amount;
	if (unit == "k") return amount << 10;
	if (unit == "M") return amount << 20;
	if (unit == "G") return amount << 30;
	throw ScriptError("'" + text + "' is not a size; write it as bytes, or with k, M or G");
}

Api::Priority priority(const std::string& word)
{
	if (word == "paused") return Api::Priority::Paused;
	if (word == "low") return Api::Priority::Low;
	if (word == "normal") return Api::Priority::Normal;
	if (word == "high") return Api::Priority::High;
	throw ScriptError("'" + word + "' is not a priority; it is paused, low, normal or high");
}

/* splitmix64, so a file is the same bytes on every machine for the same seed. */
class Pattern {
	public:
		explicit Pattern(uint64_t seed) : state(seed) { }

		uint64_t next()
		{
			uint64_t z = (state += 0x9e3779b97f4a7c15ull);
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			return z ^ (z >> 31);
		}

	private:
		uint64_t state;
};

bool writePattern(const std::string& path, uint64_t size, uint64_t seed, std::string& why)
{
	std::error_code error;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
	if (error)
	{
		why = error.message();
		return false;
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
	{
		why = "cannot be written";
		return false;
	}

	Pattern pattern(seed);
	std::vector<char> block(64 * 1024);

	for (uint64_t left = size; left > 0;)
	{
		const size_t length = static_cast<size_t>(std::min<uint64_t>(left, block.size()));
		for (size_t n = 0; n < length; n += 8)
		{
			const uint64_t word = pattern.next();
			std::memcpy(block.data() + n, &word, std::min<size_t>(8, length - n));
		}
		out.write(block.data(), static_cast<std::streamsize>(length));
		left -= length;
	}

	out.close();
	if (!out)
	{
		why = "could not be written in full";
		return false;
	}
	return true;
}

/* Empty where the two hold the same bytes; otherwise why not. */
std::string compareFiles(const std::string& path, const std::string& other)
{
	std::error_code error;
	const uint64_t size = std::filesystem::file_size(path, error);
	if (error) return "'" + path + "': " + error.message();
	const uint64_t otherSize = std::filesystem::file_size(other, error);
	if (error) return "'" + other + "': " + error.message();

	if (size != otherSize)
		return "'" + path + "' is " + std::to_string(size) + " bytes and '" + other + "' "
		       + std::to_string(otherSize);

	std::ifstream a(path, std::ios::binary);
	std::ifstream b(other, std::ios::binary);
	if (!a || !b) return "cannot read both '" + path + "' and '" + other + "'";

	std::vector<char> left(64 * 1024);
	std::vector<char> right(left.size());

	for (uint64_t offset = 0; offset < size;)
	{
		const size_t length = static_cast<size_t>(std::min<uint64_t>(size - offset, left.size()));
		a.read(left.data(), static_cast<std::streamsize>(length));
		b.read(right.data(), static_cast<std::streamsize>(length));
		if (!a || !b) return "cannot read both '" + path + "' and '" + other + "'";

		const auto differ = std::mismatch(left.begin(), left.begin() + length, right.begin());
		if (differ.first != left.begin() + length)
			return "'" + path + "' and '" + other + "' differ at byte "
			       + std::to_string(offset + (differ.first - left.begin()));

		offset += length;
	}

	return std::string();
}

/* Relative to the core's home where it names one: these run before the cores do. */
Step parseFile(const Line& line)
{
	line.expect(2, 3);
	const std::string path = line.args[0];
	const uint64_t size = parseSize(line.args[1]);

	uint64_t seed = 0;
	if (line.args.size() == 3)
	{
		const std::string& text = line.args[2];
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seed);
		if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
			throw ScriptError("'" + text + "' is not a seed");
	}

	return [line, path, size, seed](Session& session) {
		const std::string where = session.resolve(line, path);
		std::string why;
		if (!writePattern(where, size, seed, why)) return session.fail(line, "'" + where + "' " + why);
		return Outcome::Next;
	};
}

Step parseShare(const Line& line)
{
	if (line.args.size() != 1 && !(line.args.size() == 3 && line.args[1] == "as"))
		throw ScriptError("wrong arguments");

	const std::string directory = line.args[0];
	const std::string core = line.requireCore();
	std::string name = line.args.size() == 3 ? line.args[2] : std::string();

	return [line, core, directory, name](Session& session) mutable {
		const std::string where = session.resolve(line, directory);

		std::error_code error;
		std::filesystem::create_directories(where, error);
		if (error) return session.fail(line, "'" + where + "': " + error.message());

		if (name.empty()) name = std::filesystem::path(where).filename().string();
		session.presetShare(core, name, where);
		return Outcome::Next;
	};
}

/* Relative to the core's home where it names one, as 'share' is. */
Step parseShareAdd(const Line& line)
{
	if (line.args.size() != 1 && !(line.args.size() == 3 && line.args[1] == "as"))
		throw ScriptError("wrong arguments");

	const std::string directory = line.args[0];
	const std::string name = line.args.size() == 3 ? line.args[2] : std::string();
	const std::string core = line.requireCore();

	return [line, core, directory, name](Session& session) {
		return session.submit(line, core,
			Cmd::AddShareFolder{session.resolve(line, directory), name, session.nextRequest()});
	};
}

Step parseShareRemove(const Line& line)
{
	line.expect(1, 1);
	const std::string directory = line.args[0];
	const std::string core = line.requireCore();

	return [line, core, directory](Session& session) {
		return session.submit(line, core,
			Cmd::RemoveShareFolder{session.resolve(line, directory), session.nextRequest()});
	};
}

Step parseShareRename(const Line& line)
{
	line.expect(2, std::numeric_limits<size_t>::max());
	const std::string directory = line.args[0];
	const std::string name = line.rest(1);
	const std::string core = line.requireCore();

	return [line, core, directory, name](Session& session) {
		return session.submit(line, core,
			Cmd::RenameShareFolder{session.resolve(line, directory), name, session.nextRequest()});
	};
}

Step parseShareStatus(const Line& line)
{
	line.expect(0, 0);
	const std::string core = line.requireCore();

	return [line, core](Session& session) {
		return session.submit(line, core, Cmd::ReportShare{session.nextRequest()});
	};
}

/* A word with a leading '-' is one a result must not hold; 'as' names the search the core numbers. */
Step parseSearch(const Line& line)
{
	line.expect(1, std::numeric_limits<size_t>::max());

	Cmd::Search command;
	std::string name;
	size_t words = line.args.size();
	if (words >= 3 && line.args[words - 2] == "as")
	{
		name = line.args[words - 1];
		words -= 2;
	}

	for (size_t n = 0; n < words; ++n)
	{
		const std::string& word = line.args[n];
		if (word.size() > 1 && word[0] == '-') command.exclude.push_back(word.substr(1));
		else command.include.push_back(word);
	}

	if (command.include.empty()) throw ScriptError("nothing to search for");
	const std::string core = line.requireCore();

	return [line, core, command, name](Session& session) mutable {
		command.request = session.nextRequest();
		if (!name.empty()) session.nameSearch(core, command.request, name);
		return session.submit(line, core, command);
	};
}

/* By the name 'search ... as' gave it, or by its number. */
Step parseSearchForget(const Line& line)
{
	line.expect(1, 1);
	const std::string which = line.args[0];
	const std::string core = line.requireCore();

	return [line, core, which](Session& session) {
		const std::optional<uint64_t> search = session.search(which);
		if (!search) return session.fail(line, "no search is known as '" + which + "'");
		return session.submit(line, core, Cmd::ForgetSearch{*search});
	};
}

Step parseAttach(const Line& line)
{
	line.expect(1, 1);
	const std::string path = line.args[0];
	const std::string core = line.requireCore();

	return [line, core, path](Session& session) {
		return session.submit(line, core, Cmd::AttachFile{session.resolve(line, path), true, false});
	};
}

/* The bytes of 'path' handed over as though pasted, its extension as their kind. */
Step parsePaste(const Line& line)
{
	line.expect(1, 1);
	const std::string path = line.args[0];
	const std::string core = line.requireCore();

	return [line, core, path](Session& session) {
		const std::string from = session.resolve(line, path);
		std::ifstream in(from, std::ios::binary);
		if (!in) return session.fail(line, "'" + from + "' cannot be read");
		std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

		const std::string extension = std::filesystem::path(from).extension().string();
		return session.submit(line, core, Cmd::AttachPasted{
			session.nextRequest(), std::move(data), extension.empty() ? extension : extension.substr(1)});
	};
}

/* A hub of '-' leaves where to look to the queue. */
Step parseMediaFetch(const Line& line)
{
	line.expect(4, 4);
	const std::string hubName = line.args[0];
	const std::string tth = line.args[1];
	const std::string name = line.args[2];
	const uint64_t size = parseSize(line.args[3]);
	const std::string core = line.requireCore();

	return [line, core, hubName, tth, name, size](Session& session) {
		Api::HubHandle handle = 0;
		if (hubName != "-")
		{
			const std::optional<Api::HubHandle> hub = session.hub(core, hubName);
			if (!hub) return session.fail(line, "core '" + core + "' has no hub '" + hubName + "' open");
			handle = *hub;
		}
		return session.submit(line, core, Cmd::FetchAttachment{handle, session.expand(tth), name, size});
	};
}

Step parseMediaPolicy(const Line& line)
{
	line.expect(2, 2);
	using Choice = Cmd::SetChatMediaFetch::Choice;
	Choice choice = Choice::Follow;
	if (line.args[1] == "always") choice = Choice::Always;
	else if (line.args[1] == "never") choice = Choice::Never;
	else if (line.args[1] != "follow") throw ScriptError("'" + line.args[1] + "' is none of follow, always, never");

	const std::string url = line.args[0];
	const std::string core = line.requireCore();
	return [line, core, url, choice](Session& session) {
		return session.submit(line, core, Cmd::SetChatMediaFetch{url, choice});
	};
}

/* A board's hub, by the name the script opened it as, or by an address. */
std::optional<std::string> boardHub(Session& session, const std::string& core, const std::string& word)
{
	if (word.find("://") != std::string::npos) return word;
	const std::optional<Api::HubHandle> hub = session.hub(core, word);
	if (!hub) return std::nullopt;
	return session.hubUrl(core, *hub);
}

template<typename Make>
Step onBoardHub(const Line& line, const std::string& hubName, Make make)
{
	const std::string core = line.requireCore();

	return [line, core, hubName, make](Session& session) {
		const std::optional<std::string> url = boardHub(session, core, hubName);
		if (!url) return session.fail(line, "core '" + core + "' has no hub '" + hubName + "' open");
		return session.submit(line, core, make(session, *url));
	};
}

Step parseBoardLoad(const Line& line)
{
	line.expect(2, 2);
	const std::string board = line.args[1];
	return onBoardHub(line, line.args[0], [board](Session&, const std::string& url) {
		return Api::Command(Cmd::LoadBoard{url, board});
	});
}

Step parseBoardSubscribe(const Line& line)
{
	line.expect(2, 3);
	if (line.args.size() == 3 && line.args[2] != "from-start")
		throw ScriptError("'" + line.args[2] + "' is not from-start");
	const std::string board = line.args[1];
	const bool fromStart = line.args.size() == 3;
	return onHub(line, line.args[0], [board, fromStart](Session&, Api::HubHandle hub) {
		return Api::Command(Cmd::SubscribeBoard{hub, board, fromStart});
	});
}

Step parseBoardLeave(const Line& line)
{
	line.expect(2, 2);
	const std::string board = line.args[1];
	return onHub(line, line.args[0], [board](Session&, Api::HubHandle hub) {
		return Api::Command(Cmd::UnsubscribeBoard{hub, board});
	});
}

Step parseBoardRead(const Line& line)
{
	line.expect(3, 4);
	if (line.args.size() == 4 && line.args[3] != "fetch") throw ScriptError("'" + line.args[3] + "' is not fetch");
	const std::string board = line.args[1];
	const std::string tth = line.args[2];
	const bool fetch = line.args.size() == 4;
	return onBoardHub(line, line.args[0], [board, tth, fetch](Session& session, const std::string& url) {
		return Api::Command(Cmd::ReadPost{url, board, session.expand(tth), fetch});
	});
}

Step parseBoardMarkRead(const Line& line)
{
	line.expect(2, 3);
	const std::string board = line.args[1];
	const std::string tth = line.args.size() == 3 ? line.args[2] : std::string();
	return onBoardHub(line, line.args[0], [board, tth](Session& session, const std::string& url) {
		return Api::Command(Cmd::MarkRead{url, board, session.expand(tth)});
	});
}

/* A new thread with a subject, or with 'reply' a reply to '$NAME' or a hash. */
Step parseBoardPost(const Line& line)
{
	line.expect(4, std::numeric_limits<size_t>::max());
	const std::string board = line.args[1];
	std::string parent;
	std::string subject;
	size_t body = 3;

	if (line.args[2] == "reply")
	{
		line.expect(5, std::numeric_limits<size_t>::max());
		parent = line.args[3];
		body = 4;
	}
	else
	{
		subject = line.args[2];
	}

	const std::string text = line.rest(body);
	return onHub(line, line.args[0], [board, parent, subject, text](Session& session, Api::HubHandle hub) {
		return Api::Command(Cmd::WritePost{
			session.nextRequest(), hub, board, session.expand(parent), subject, session.expand(text), false});
	});
}

Step parseBoardWithdraw(const Line& line)
{
	line.expect(3, 3);
	const std::string board = line.args[1];
	const std::string tth = line.args[2];
	return onHub(line, line.args[0], [board, tth](Session& session, Api::HubHandle hub) {
		return Api::Command(Cmd::WithdrawPost{hub, session.expand(tth), board});
	});
}

Step parseBoardPolicy(const Line& line)
{
	line.expect(3, 3);
	using Policy = Cmd::SetBoardPolicy::Policy;
	using Choice = Cmd::SetBoardPolicy::Choice;

	Policy policy = Policy::FetchAhead;
	if (line.args[1] == "serve") policy = Policy::Serve;
	else if (line.args[1] != "fetch-ahead") throw ScriptError("'" + line.args[1] + "' is neither fetch-ahead nor serve");

	Choice choice = Choice::Follow;
	if (line.args[2] == "always") choice = Choice::Always;
	else if (line.args[2] == "never") choice = Choice::Never;
	else if (line.args[2] != "follow") throw ScriptError("'" + line.args[2] + "' is none of follow, always, never");

	return onBoardHub(line, line.args[0], [policy, choice](Session&, const std::string& url) {
		return Api::Command(Cmd::SetBoardPolicy{url, policy, choice});
	});
}

/* The result is one this core has heard, so the line waits for it first. */
Step parseDownload(const Line& line)
{
	line.expect(2, 6);

	const std::string what = line.args[1];
	std::string nick;
	std::string to;

	for (size_t n = 2; n < line.args.size(); n += 2)
	{
		if (n + 1 >= line.args.size()) throw ScriptError("'" + line.args[n] + "' wants a value");
		if (line.args[n] == "from") nick = line.args[n + 1];
		else if (line.args[n] == "to") to = line.args[n + 1];
		else throw ScriptError("'" + line.args[n] + "' is neither from nor to");
	}

	const std::string core = line.requireCore();
	const std::string hubName = line.args[0];

	return [line, core, hubName, what, nick, to](Session& session) {
		const std::optional<Api::HubHandle> hub = session.hub(core, hubName);
		if (!hub) return session.fail(line, "core '" + core + "' has no hub '" + hubName + "' open");

		const std::optional<Api::SearchHit> hit = session.searchHit(core, *hub, what, nick);
		if (!hit)
			return session.fail(line, "core '" + core + "' has heard no search result for '" + what + "'"
			                          + (nick.empty() ? std::string() : " from " + nick));

		const std::string name = hit->name.substr(hit->name.find_last_of("/\\") + 1);
		const std::string target = session.resolve(line, to.empty() ? "downloads/" + name : to);

		std::error_code error;
		std::filesystem::create_directories(std::filesystem::path(target).parent_path(), error);
		if (error) return session.fail(line, "'" + target + "': " + error.message());

		return session.submit(line, core, Cmd::EnqueueFromSearch{
			target, hit->tth, hit->size, hit->hubUrl, hit->user.sourceId, hit->name});
	};
}

Step parseBrowse(const Line& line)
{
	line.expect(2, 2);
	const std::string who = line.args[1];
	const std::string core = line.requireCore();
	const std::string hubName = line.args[0];

	return [line, core, hubName, who](Session& session) {
		const std::optional<Api::HubHandle> hub = session.hub(core, hubName);
		if (!hub) return session.fail(line, "core '" + core + "' has no hub '" + hubName + "' open");

		const std::optional<Api::User> user = session.user(core, *hub, who);
		const std::string source = user ? user->sourceId : who;
		return session.submit(line, core, Cmd::RequestFilelist{session.hubUrl(core, *hub), source});
	};
}

Step parseTransfers(const Line& line)
{
	line.expect(0, 0);
	const std::string core = line.requireCore();

	return [line, core](Session& session) {
		return session.submit(line, core, Cmd::SnapshotTransfers{session.nextRequest()});
	};
}

Step parsePriority(const Line& line)
{
	line.expect(2, 2);
	const std::string path = line.args[0];
	const Api::Priority level = priority(line.args[1]);
	const std::string core = line.requireCore();

	return [line, core, path, level](Session& session) {
		return session.submit(line, core, Cmd::SetPriority{session.resolve(line, path), level});
	};
}

Step parseDequeue(const Line& line)
{
	line.expect(1, 2);
	if (line.args.size() == 2 && line.args[1] != "delete-partial")
		throw ScriptError("'" + line.args[1] + "' is not delete-partial");

	const std::string path = line.args[0];
	const bool deletePartial = line.args.size() == 2;
	const std::string core = line.requireCore();

	return [line, core, path, deletePartial](Session& session) {
		return session.submit(line, core, Cmd::RemoveFromQueue{session.resolve(line, path), deletePartial});
	};
}

Step parseExpectFile(const Line& line)
{
	line.expect(3, 3);
	const std::string path = line.args[0];
	const std::string how = line.args[1];
	const std::string what = line.args[2];

	if (how == "same-as")
	{
		return [line, path, what](Session& session) {
			const std::string why = compareFiles(session.resolve(line, path), session.resolve(line, what));
			return why.empty() ? Outcome::Next : session.fail(line, why);
		};
	}

	if (how == "size")
	{
		const uint64_t size = parseSize(what);
		return [line, path, size](Session& session) {
			const std::string where = session.resolve(line, path);
			std::error_code error;
			const uint64_t found = std::filesystem::file_size(where, error);
			if (error) return session.fail(line, "'" + where + "': " + error.message());
			if (found != size)
				return session.fail(line, "'" + where + "' is " + std::to_string(found) + " bytes");
			return Outcome::Next;
		};
	}

	throw ScriptError("'" + how + "' is neither same-as nor size");
}

/* The event a line names and the fields it tests, from 'line.args[0]' on. */
QuickDC::Cli::Expectation expectation(const Line& line, std::optional<std::chrono::milliseconds>& timeout)
{
	line.expect(1, std::numeric_limits<size_t>::max());

	const std::vector<std::string>& names = QuickDC::Cli::eventNames();
	if (std::find(names.begin(), names.end(), line.args[0]) == names.end())
		throw ScriptError("there is no event '" + line.args[0] + "'");

	QuickDC::Cli::Expectation expected;
	expected.core = line.addressed ? line.core : std::string();
	expected.event = line.args[0];

	for (size_t n = 1; n < line.args.size(); ++n)
	{
		const std::string& arg = line.args[n];

		if (arg == "remember")
		{
			if (n + 1 >= line.args.size()) throw ScriptError("remember wants NAME=field");
			const std::string& what = line.args[++n];
			const size_t equals = what.find('=');
			if (equals == std::string::npos || equals == 0 || equals + 1 == what.size())
				throw ScriptError("'" + what + "' is not NAME=field");
			expected.remember.emplace_back(what.substr(0, equals), what.substr(equals + 1));
			continue;
		}

		const size_t equals = arg.find('=');
		if (equals == std::string::npos || equals == 0)
			throw ScriptError("'" + arg + "' is not field=value");

		const bool differs = arg[equals - 1] == '!';
		const std::string field = arg.substr(0, differs ? equals - 1 : equals);
		const std::string value = arg.substr(equals + 1);
		if (field.empty()) throw ScriptError("'" + arg + "' is not field=value");

		if (field == "timeout" && !differs) timeout = QuickDC::Cli::parseDuration(value);
		else expected.fields.push_back({field, value, differs});
	}

	return expected;
}

Step parseWait(const Line& line)
{
	std::optional<std::chrono::milliseconds> timeout;
	QuickDC::Cli::Expectation expected = expectation(line, timeout);

	return [line, expected, timeout](Session& session) mutable {
		expected.timeout = timeout.value_or(session.options().waitTimeout);
		return session.wait(line, expected);
	};
}

Step parseExpectNo(const Line& line)
{
	std::optional<std::chrono::milliseconds> timeout;
	const QuickDC::Cli::Expectation expected = expectation(line, timeout);
	if (timeout || !expected.remember.empty())
		throw ScriptError("expect-no waits for nothing and remembers nothing");

	return [line, expected](Session& session) { return session.expectNone(line, expected); };
}

Step parseHublistRefresh(const Line& line)
{
	line.expect(0, 1);
	if (line.args.size() == 1 && line.args[0] != "force")
		throw ScriptError("'" + line.args[0] + "' is not force");

	const bool force = line.args.size() == 1;
	const std::string core = line.requireCore();

	return [line, core, force](Session& session) {
		return session.submit(line, core, Cmd::RefreshHubDirectory{force});
	};
}

int parsePort(const std::string& text)
{
	int port = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
	if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || port <= 0 || port > 65535)
		throw ScriptError("'" + text + "' is not a port");
	return port;
}

/* Relative to the run's home where it is not absolute. */
Step parseServe(const Line& line)
{
	line.expect(2, 2);
	const int port = parsePort(line.args[0]);
	const std::string directory = line.args[1];

	return [line, port, directory](Session& session) {
		return session.serve(line, port, session.resolve(line, directory));
	};
}

Step parseExpectServed(const Line& line)
{
	line.expect(2, 2);
	const int port = parsePort(line.args[0]);

	int count = 0;
	const std::string& text = line.args[1];
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
	if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || count < 0)
		throw ScriptError("'" + text + "' is not a count");

	return [line, port, count](Session& session) {
		const std::optional<int> served = session.served(port);
		if (!served) return session.fail(line, "nothing is served on port " + std::to_string(port));
		if (*served != count)
			return session.fail(line, "port " + std::to_string(port) + " served " + std::to_string(*served)
				+ " files, not " + std::to_string(count));
		return Outcome::Next;
	};
}

Step parseRestart(const Line& line)
{
	line.expect(0, 0);
	const std::string core = line.requireCore();
	return [line, core](Session& session) { return session.restart(line, core); };
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
		{"mode",       "mode <automatic|advanced|passive> [upnp] [force-active]", parseMode},
		{"state",      "state",                                              parseState},
		{"favourite-add", "favourite-add <url>",                             parseNaming<Cmd::AddFavouriteHub>},
		{"favourite-remove", "favourite-remove <url>",                       parseNaming<Cmd::RemoveFavouriteHub>},
		{"recent-forget", "recent-forget <url>",                             parseNaming<Cmd::ForgetRecentHub>},
		{"recent-clear", "recent-clear",                                     parseBare<Cmd::ClearRecentHubs>},
		{"completed-forget", "completed-forget <path>",                      parseCompletedForget},
		{"completed-clear", "completed-clear",                               parseBare<Cmd::ClearCompletedDownloads>},
		{"hublist-add", "hublist-add <url>",                                 parseNaming<Cmd::AddHubListSource>},
		{"hublist-remove", "hublist-remove <url>",                           parseNaming<Cmd::RemoveHubListSource>},
		{"hublist-reset", "hublist-reset",                                   parseBare<Cmd::ResetHubListSources>},
		{"hublist-refresh", "hublist-refresh [force]",                       parseHublistRefresh},
		{"serve",      "serve <port> <directory>",                           parseServe},
		{"expect-served", "expect-served <port> <count>",                    parseExpectServed},
		{"restart",    "restart",                                            parseRestart},
		{"open",       "open <url> [as <name>]",                             parseOpen},
		{"close",      "close <hub>",                                        parseClose},
		{"connect",    "connect <hub>",                                      parseConnect},
		{"disconnect", "disconnect <hub>",                                   parseDisconnect},
		{"chat",       "chat <hub> <text...>",                               parseChat},
		{"chat-md",    "chat-md <hub> <markdown...>",                        parseChatMarkdown},
		{"pm",         "pm <hub> <nick|id> <text...>",                       parsePrivate},
		{"password",   "password <hub> <password>",                          parsePassword},
		{"file",       "file <path> <size>[k|M|G] [seed]",                   parseFile, true},
		{"share",      "share <directory> [as <name>]",                      parseShare, true},
		{"share-add",  "share-add <directory> [as <name>]",                  parseShareAdd},
		{"share-remove", "share-remove <directory>",                         parseShareRemove},
		{"share-rename", "share-rename <directory> <name...>",               parseShareRename},
		{"share-status", "share-status",                                     parseShareStatus},
		{"attach",     "attach <path>",                                      parseAttach},
		{"paste",      "paste <path>",                                       parsePaste},
		{"media-fetch", "media-fetch <hub|-> <tth> <name> <size>",           parseMediaFetch},
		{"media-policy", "media-policy <hub url> <follow|always|never>",     parseMediaPolicy},
		{"board-load", "board-load <hub|url> <board>",                       parseBoardLoad},
		{"board-subscribe", "board-subscribe <hub> <board> [from-start]",    parseBoardSubscribe},
		{"board-leave", "board-leave <hub> <board>",                         parseBoardLeave},
		{"board-read", "board-read <hub|url> <board> <tth> [fetch]",         parseBoardRead},
		{"board-mark-read", "board-mark-read <hub|url> <board> [tth]",       parseBoardMarkRead},
		{"board-post", "board-post <hub> <board> <subject>|reply <tth> <body...>", parseBoardPost},
		{"board-withdraw", "board-withdraw <hub> <board> <tth>",             parseBoardWithdraw},
		{"board-policy", "board-policy <hub|url> <fetch-ahead|serve> <follow|always|never>", parseBoardPolicy},
		{"search",     "search <word|-word ...> [as <name>]",                parseSearch},
		{"search-forget", "search-forget <name|number>",                     parseSearchForget},
		{"download",   "download <hub> <name|tth> [from <nick>] [to <path>]", parseDownload},
		{"browse",     "browse <hub> <nick|id>",                             parseBrowse},
		{"transfers",  "transfers",                                          parseTransfers},
		{"priority",   "priority <path> <paused|low|normal|high>",           parsePriority},
		{"dequeue",    "dequeue <path> [delete-partial]",                    parseDequeue},
		{"expect-file", "expect-file <path> same-as <path> | size <size>",   parseExpectFile},
		{"wait",       "wait <Event> [field=value|field!=value ...] [remember NAME=field ...] [timeout=<N>s|<N>ms]", parseWait},
		{"expect-no",  "expect-no <Event> [field=value|field!=value ...]",   parseExpectNo},
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
