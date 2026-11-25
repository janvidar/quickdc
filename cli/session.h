/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_SESSION_H
#define HAVE_QUICKDC_CLI_SESSION_H

#include "fileserver.h"
#include "json.h"
#include "runner.h"
#include "script.h"

#include "api/protocol.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace QuickDC::Cli {

struct Options {
	/** What failures name the script by. */
	std::string script = "-";
	bool quiet = false;
	bool keepGoing = false;
	std::chrono::milliseconds waitTimeout = std::chrono::seconds(10);
};

/** One field=value (or field!=value) test; a value '$NAME' is what was remembered as NAME. */
struct FieldTest {
	std::string path;
	std::string value;
	bool differs = false;
};

/** What a wait is waiting for. */
struct Expectation {
	/** Empty for any core. */
	std::string core;
	std::string event;
	std::vector<FieldTest> fields;
	/** Names to remember the matched event's fields by, and the paths of those fields. */
	std::vector<std::pair<std::string, std::string>> remember;
	std::chrono::milliseconds timeout{0};

	std::string describe() const;
};

/**
 * What a script runs against: the cores, the names it gave its hubs, and every
 * event heard so far that no wait has claimed.
 */
class Session {
	public:
		Session(Runner& runner, Options options);

		const Options& options() const { return settings; }

		/** Report a failure of the line being run. */
		Outcome fail(const Line& line, const std::string& why);

		/** Hand 'command' to 'core', or fail the line. */
		Outcome submit(const Line& line, const std::string& core, QuickDC::Api::Command command);

		/** Print and keep every event that arrives before 'deadline'. */
		void pump(std::chrono::steady_clock::time_point deadline);

		/** Take the oldest unclaimed event that meets 'expected', waiting for one if need be. */
		Outcome wait(const Line& line, const Expectation& expected);

		/** Fail if any event heard so far, claimed or not, meets 'expected'. */
		Outcome expectNone(const Line& line, const Expectation& expected);

		/** Say something about the run, which is never mixed into the events. */
		void note(const std::string& text) const;

		/* The handles a script's hub names stand for, one numbering per core. */
		QuickDC::Api::HubHandle openHub(const std::string& core, const std::string& name,
		                                const std::string& url);
		std::optional<QuickDC::Api::HubHandle> hub(const std::string& core, const std::string& name) const;
		void forgetHub(const std::string& core, const std::string& name);

		/** Whoever on 'hub' was last seen with the nickname 'nick'. */
		std::optional<QuickDC::Api::User> user(const std::string& core, QuickDC::Api::HubHandle hub,
		                                       const std::string& nick) const;

		/** The address the core knows 'hub' by: what it said, or else what was opened. */
		std::string hubUrl(const std::string& core, QuickDC::Api::HubHandle hub) const;

		/**
		 * The latest search result 'core' heard on 'hub' for a file named 'what'
		 * (its whole name or the last part of it) or with that hash, and from the
		 * user 'nick' where that is not empty.
		 */
		std::optional<QuickDC::Api::SearchHit> searchHit(const std::string& core, QuickDC::Api::HubHandle hub,
		                                                 const std::string& what, const std::string& nick) const;

		uint64_t nextRequest() { return ++requests; }

		/** Remember the number SearchSent gives the search sent as 'request' as 'name'. */
		void nameSearch(const std::string& core, uint64_t request, const std::string& name);

		/** 'text' with each '$NAME' that names something remembered replaced by it. */
		std::string expand(const std::string& text) const;

		/** The search remembered as 'which', or 'which' read as a number. */
		std::optional<uint64_t> search(const std::string& which) const;

		/** The directory the cores' homes are made in, one per core, named after it. */
		void setBase(std::string directory) { base = std::move(directory); }

		/**
		 * 'path' as the line means it: as given where it is absolute, otherwise
		 * under the home of the core the line names, or of the run where it names none.
		 */
		std::string resolve(const Line& line, const std::string& path) const;

		/** Have 'core' share 'directory' as 'name' from the start. */
		void presetShare(const std::string& core, const std::string& name, const std::string& directory);

		/** Take a setting as though 'core''s files held it when it starts; every core for "". */
		void preset(const std::string& core, QuickDC::Api::SettingsStore store,
		            const std::string& group, const std::string& key, const std::string& value);

		QuickDC::Api::SettingsSnapshot presets(const std::string& core) const;

		/**
		 * Stop 'core' and build it again over the same home, with the same presets,
		 * forgetting what it said and the hubs it had open.
		 */
		Outcome restart(const Line& line, const std::string& core);

		/** Serve the files under 'directory' over HTTP on the loopback address at 'port'. */
		Outcome serve(const Line& line, int port, const std::string& directory);

		/** How many files the server on 'port' has handed out; nothing where there is none. */
		std::optional<int> served(int port) const;

	private:
		struct Heard {
			std::string core;
			Json::Value event;
			std::string line;
			bool claimed = false;
		};

		struct Hubs {
			QuickDC::Api::HubHandle last = 0;
			std::map<std::string, QuickDC::Api::HubHandle> byName;
			std::map<QuickDC::Api::HubHandle, std::string> names;
			std::map<QuickDC::Api::HubHandle, std::map<std::string, QuickDC::Api::User>> users;
			std::map<QuickDC::Api::HubHandle, std::string> urls;
			std::deque<std::pair<QuickDC::Api::HubHandle, QuickDC::Api::SearchHit>> hits;
			std::map<uint64_t, std::string> searchNames;
			int shares = 0;
		};

		void take(const Delivery& delivery);
		void observe(Hubs& hubs, const QuickDC::Api::Event& event);
		bool matches(const Heard& heard, const Expectation& expected, bool claimedToo = false) const;
		bool remember(const Line& line, const Heard& heard, const Expectation& expected);
		std::string valueOf(const FieldTest& test) const;
		void print(const std::string& line) const;

		Runner& runner;
		Options settings;
		std::map<std::string, Hubs> cores;
		std::deque<Heard> backlog;
		std::map<std::string, QuickDC::Api::SettingsSnapshot> presetsByCore;
		uint64_t requests = 0;
		std::string base;
		std::map<std::string, std::string> remembered;
		std::map<int, std::unique_ptr<FileServer>> servers;
};

}

#endif // HAVE_QUICKDC_CLI_SESSION_H
