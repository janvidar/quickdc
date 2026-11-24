/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_RUNNER_H
#define HAVE_QUICKDC_CLI_RUNNER_H

#include "api/protocol.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace QuickDC::Cli {

/** An event, and the name of the core that told it. */
struct Delivery {
	std::string core;
	QuickDC::Api::Event event;
};

/**
 * The cores, each run on a thread of its own, and what they say.
 *
 * Everything here is called from one thread, the script's. The only place in the
 * program that sees a core: the rest has commands and events.
 */
class Runner {
	public:
		Runner();
		~Runner();

		Runner(const Runner&) = delete;
		Runner& operator=(const Runner&) = delete;

		/**
		 * Build the core 'name' over the directory 'home', with 'presets' taken
		 * as what its files hold, and wait until it takes commands. False, with
		 * 'failure' saying why, if it could not be built.
		 */
		bool start(const std::string& name, const std::string& home,
		           const QuickDC::Api::SettingsSnapshot& presets, std::string& failure);

		/** False where the core is not running, or refused the command. */
		bool submit(const std::string& name, QuickDC::Api::Command command);

		/** The next event from any core, in the order each core told them. */
		std::optional<Delivery> next(std::chrono::steady_clock::time_point deadline);

		/** Stop every core and wait for its thread. The events they leave stay queued. */
		void stop();

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
};

}

#endif // HAVE_QUICKDC_CLI_RUNNER_H
