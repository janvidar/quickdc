/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "session.h"
#include "eventjson.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace Api = QuickDC::Api;

namespace {

/* A run that waits on nothing must not grow without bound. */
const size_t BACKLOG_LIMIT = 100000;
const size_t HITS_LIMIT = 10000;

std::string lastPart(const std::string& name)
{
	const size_t slash = name.find_last_of("/\\");
	return slash == std::string::npos ? name : name.substr(slash + 1);
}

}

std::string QuickDC::Cli::Expectation::describe() const
{
	std::string out = core.empty() ? std::string() : "@" + core + " ";
	out += event;
	for (const FieldTest& test : fields) out += " " + test.path + (test.differs ? "!=" : "=") + test.value;
	return out;
}

QuickDC::Cli::Session::Session(Runner& runner, Options options)
	: runner(runner)
	, settings(std::move(options))
{
}

QuickDC::Cli::Outcome QuickDC::Cli::Session::fail(const Line& line, const std::string& why)
{
	std::fprintf(stderr, "qdc: %s:%d: %s: %s\n", settings.script.c_str(), line.number,
	             line.source.c_str(), why.c_str());
	return Outcome::Failed;
}

QuickDC::Cli::Outcome QuickDC::Cli::Session::submit(const Line& line, const std::string& core, Api::Command command)
{
	if (!runner.submit(core, std::move(command)))
		return fail(line, "core '" + core + "' is not running");
	return Outcome::Next;
}

void QuickDC::Cli::Session::note(const std::string& text) const
{
	std::fprintf(stderr, "%s\n", text.c_str());
}

void QuickDC::Cli::Session::print(const std::string& line) const
{
	std::fwrite(line.data(), 1, line.size(), stdout);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

void QuickDC::Cli::Session::pump(std::chrono::steady_clock::time_point deadline)
{
	while (std::optional<Delivery> delivery = runner.next(deadline)) take(*delivery);
}

void QuickDC::Cli::Session::take(const Delivery& delivery)
{
	Hubs& hubs = cores[delivery.core];
	observe(hubs, delivery.event);

	const HubNamer name = [&hubs](Api::HubHandle handle) -> Json::Value {
		const auto found = hubs.names.find(handle);
		if (found == hubs.names.end()) return Json::Value(handle);
		return Json::Value(found->second);
	};

	Heard heard;
	heard.event = describe(delivery.core, delivery.event, name);
	heard.line = heard.event.serialized();

	if (!settings.quiet) print(heard.line);

	backlog.push_back(std::move(heard));
	while (backlog.size() > BACKLOG_LIMIT) backlog.pop_front();
}

void QuickDC::Cli::Session::observe(Hubs& hubs, const Api::Event& event)
{
	if (const auto* updated = event.as<Api::Events::UserUpdated>())
	{
		if (!updated->user.nick.empty())
			hubs.users[updated->hub][updated->user.nick] = updated->user;
	}
	else if (const auto* left = event.as<Api::Events::UserLeft>())
	{
		hubs.users[left->hub].erase(left->user.nick);
	}
	else if (const auto* cleared = event.as<Api::Events::UsersCleared>())
	{
		hubs.users.erase(cleared->hub);
	}
	else if (const auto* result = event.as<Api::Events::SearchResult>())
	{
		if (!result->hit.hubUrl.empty()) hubs.urls[result->hub] = result->hit.hubUrl;
		hubs.hits.emplace_back(result->hub, result->hit);
		while (hubs.hits.size() > HITS_LIMIT) hubs.hits.pop_front();
	}
	else if (const auto* chat = event.as<Api::Events::ChatReceived>())
	{
		if (!chat->line.hubUrl.empty()) hubs.urls[chat->hub] = chat->line.hubUrl;
	}
}

bool QuickDC::Cli::Session::matches(const Heard& heard, const Expectation& expected, bool claimedToo) const
{
	if (heard.claimed && !claimedToo) return false;

	const Json::Value* event = heard.event.find("event");
	if (!event || event->text() != expected.event) return false;

	if (!expected.core.empty())
	{
		const Json::Value* core = heard.event.find("core");
		if (!core || core->text() != expected.core) return false;
	}

	std::vector<const Json::Value*> found;
	for (const FieldTest& test : expected.fields)
	{
		const std::string value = valueOf(test);
		found.clear();
		heard.event.findAll(test.path, found);
		const bool equal = std::any_of(found.begin(), found.end(),
		                               [&value](const Json::Value* field) { return field->text() == value; });
		if (test.differs ? (found.empty() || equal) : !equal) return false;
	}

	return true;
}

QuickDC::Cli::Outcome QuickDC::Cli::Session::wait(const Line& line, const Expectation& expected)
{
	const auto deadline = std::chrono::steady_clock::now() + expected.timeout;

	size_t looked = 0;

	while (true)
	{
		for (; looked < backlog.size(); ++looked)
		{
			Heard& heard = backlog[looked];
			if (!matches(heard, expected)) continue;

			heard.claimed = true;
			if (settings.quiet) print(heard.line);
			return remember(line, heard, expected) ? Outcome::Next : Outcome::Failed;
		}

		std::optional<Delivery> delivery = runner.next(deadline);
		if (!delivery) break;

		const size_t before = backlog.size();
		take(*delivery);

		/* Trimmed from the front, so what was looked at already has moved. */
		const size_t dropped = before + 1 - backlog.size();
		looked = looked > dropped ? looked - dropped : 0;
	}

	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(expected.timeout);
	return fail(line, "timed out after " + std::to_string(ms.count()) + "ms waiting for "
	                  + expected.describe());
}

QuickDC::Cli::Outcome QuickDC::Cli::Session::expectNone(const Line& line, const Expectation& expected)
{
	pump(std::chrono::steady_clock::now());

	for (const Heard& heard : backlog)
		if (matches(heard, expected, true))
			return fail(line, "heard " + heard.line);

	return Outcome::Next;
}

std::string QuickDC::Cli::Session::valueOf(const FieldTest& test) const
{
	if (test.value.size() < 2 || test.value[0] != '$') return test.value;

	const auto found = remembered.find(test.value.substr(1));
	return found == remembered.end() ? test.value : found->second;
}

bool QuickDC::Cli::Session::remember(const Line& line, const Heard& heard, const Expectation& expected)
{
	for (const auto& [name, path] : expected.remember)
	{
		const Json::Value* field = heard.event.find(path);
		if (!field)
		{
			fail(line, "the event has no '" + path + "' to remember as " + name);
			return false;
		}
		remembered[name] = field->text();
	}
	return true;
}

Api::HubHandle QuickDC::Cli::Session::openHub(const std::string& core, const std::string& name,
                                               const std::string& url)
{
	Hubs& hubs = cores[core];
	const Api::HubHandle handle = ++hubs.last;
	hubs.byName[name] = handle;
	hubs.names[handle] = name;
	hubs.urls[handle] = url;
	return handle;
}

std::string QuickDC::Cli::Session::hubUrl(const std::string& core, Api::HubHandle hub) const
{
	const auto found = cores.find(core);
	if (found == cores.end()) return std::string();

	const auto url = found->second.urls.find(hub);
	return url == found->second.urls.end() ? std::string() : url->second;
}

std::optional<Api::SearchHit> QuickDC::Cli::Session::searchHit(const std::string& core, Api::HubHandle hub,
                                                               const std::string& what,
                                                               const std::string& nick) const
{
	const auto found = cores.find(core);
	if (found == cores.end()) return std::nullopt;

	const auto& hits = found->second.hits;
	for (auto at = hits.rbegin(); at != hits.rend(); ++at)
	{
		const auto& [from, hit] = *at;
		if (from != hub) continue;
		if (!nick.empty() && hit.user.nick != nick) continue;
		if (hit.tth == what || hit.name == what || lastPart(hit.name) == what) return hit;
	}

	return std::nullopt;
}

std::string QuickDC::Cli::Session::resolve(const Line& line, const std::string& path) const
{
	const std::filesystem::path given(path);
	if (given.is_absolute()) return given.lexically_normal().string();

	std::filesystem::path under(base);
	if (!line.core.empty()) under /= line.core;
	return (under / given).lexically_normal().string();
}

void QuickDC::Cli::Session::presetShare(const std::string& core, const std::string& name,
                                        const std::string& directory)
{
	const int index = cores[core].shares++;
	preset(core, Api::SettingsStore::Application, "Shares", "Share " + std::to_string(index),
	       name + ":" + directory);
	preset(core, Api::SettingsStore::Application, "Shares", "Total shares", std::to_string(index + 1));
}

std::optional<Api::HubHandle> QuickDC::Cli::Session::hub(const std::string& core, const std::string& name) const
{
	const auto found = cores.find(core);
	if (found == cores.end()) return std::nullopt;

	const auto handle = found->second.byName.find(name);
	if (handle == found->second.byName.end()) return std::nullopt;
	return handle->second;
}

void QuickDC::Cli::Session::forgetHub(const std::string& core, const std::string& name)
{
	/* The handle keeps its name, for what the session says as it closes. */
	cores[core].byName.erase(name);
}

std::optional<Api::User> QuickDC::Cli::Session::user(const std::string& core, Api::HubHandle hub,
                                                     const std::string& nick) const
{
	const auto found = cores.find(core);
	if (found == cores.end()) return std::nullopt;

	const auto users = found->second.users.find(hub);
	if (users == found->second.users.end()) return std::nullopt;

	const auto user = users->second.find(nick);
	if (user == users->second.end()) return std::nullopt;
	return user->second;
}

void QuickDC::Cli::Session::preset(const std::string& core, Api::SettingsStore store,
                          const std::string& group, const std::string& key, const std::string& value)
{
	Api::SettingsSnapshot& held = presetsByCore[core];
	Api::SettingsGroups& groups = store == Api::SettingsStore::Session ? held.session : held.application;
	groups[group][key] = value;
}

Api::SettingsSnapshot QuickDC::Cli::Session::presets(const std::string& core) const
{
	Api::SettingsSnapshot out;

	/* Every core's first, so a core's own outranks them. */
	for (const std::string& from : {std::string(), core})
	{
		const auto found = presetsByCore.find(from);
		if (found == presetsByCore.end()) continue;

		for (const auto& [group, values] : found->second.application)
			for (const auto& [key, value] : values) out.application[group][key] = value;
		for (const auto& [group, values] : found->second.session)
			for (const auto& [key, value] : values) out.session[group][key] = value;
	}

	return out;
}
