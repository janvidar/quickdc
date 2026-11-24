/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_SESSION_H
#define HAVE_QUICKDC_CLI_SESSION_H

#include "json.h"
#include "runner.h"
#include "script.h"

#include "api/protocol.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
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

/** What a wait is waiting for. */
struct Expectation {
	/** Empty for any core. */
	std::string core;
	std::string event;
	std::vector<std::pair<std::string, std::string>> fields;
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

		/** Say something about the run, which is never mixed into the events. */
		void note(const std::string& text) const;

		/* The handles a script's hub names stand for, one numbering per core. */
		QuickDC::Api::HubHandle openHub(const std::string& core, const std::string& name);
		std::optional<QuickDC::Api::HubHandle> hub(const std::string& core, const std::string& name) const;
		void forgetHub(const std::string& core, const std::string& name);

		/** Whoever on 'hub' was last seen with the nickname 'nick'. */
		std::optional<std::string> userId(const std::string& core, QuickDC::Api::HubHandle hub,
		                                  const std::string& nick) const;

		uint64_t nextRequest() { return ++requests; }

		/** Take a setting as though 'core''s files held it when it starts; every core for "". */
		void preset(const std::string& core, QuickDC::Api::SettingsStore store,
		            const std::string& group, const std::string& key, const std::string& value);

		QuickDC::Api::SettingsSnapshot presets(const std::string& core) const;

	private:
		struct Heard {
			Json::Value event;
			std::string line;
			bool claimed = false;
		};

		struct Hubs {
			QuickDC::Api::HubHandle last = 0;
			std::map<std::string, QuickDC::Api::HubHandle> byName;
			std::map<QuickDC::Api::HubHandle, std::string> names;
			std::map<QuickDC::Api::HubHandle, std::map<std::string, std::string>> users;
		};

		void take(const Delivery& delivery);
		void observe(Hubs& hubs, const QuickDC::Api::Event& event);
		bool matches(const Heard& heard, const Expectation& expected) const;
		void print(const std::string& line) const;

		Runner& runner;
		Options settings;
		std::map<std::string, Hubs> cores;
		std::deque<Heard> backlog;
		std::map<std::string, QuickDC::Api::SettingsSnapshot> presetsByCore;
		uint64_t requests = 0;
};

}

#endif // HAVE_QUICKDC_CLI_SESSION_H
