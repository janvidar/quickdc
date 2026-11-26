/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "config/preferences.h"
#include "config/settingskeeper.h"

#include <samurai/eventloop.h>
#include <samurai/io/file.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/timer.h>

#include <chrono>
#include <stdio.h>
#include <string>
#include <unistd.h>

static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_keeper_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

static std::string readFile(const std::string& path)
{
	FILE* fh = fopen(path.c_str(), "rb");
	if (!fh) return std::string();
	std::string out;
	char buf[4096];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), fh)) > 0) out.append(buf, got);
	fclose(fh);
	return out;
}

template<typename Fn>
static bool pump(Samurai::EventLoop& loop, Fn done, int timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		loop.monitor().wait(10);
		loop.timers().process();
	}
	return true;
}

class Stores
{
	public:
		Stores() : configPath(scratch("q.conf")), sessionPath(scratch("s.conf")),
		           config(configPath), session(sessionPath) { }
		~Stores()
		{
			Samurai::IO::File(configPath).remove();
			Samurai::IO::File(sessionPath).remove();
		}

		std::string configPath;
		std::string sessionPath;
		QuickDC::Preferences config;
		QuickDC::Preferences session;
};

EXO_TEST(settingskeeper_a_change_is_written_once_left_alone,
{
	Samurai::EventLoop loop;
	Stores s;
	QuickDC::SettingsKeeper keeper(loop, s.config, s.session, std::chrono::milliseconds(50));

	int writes = 0;
	keeper.written = [&](const QuickDC::Preferences& store, bool ok) {
		if (&store == &s.config && ok) writes++;
	};

	s.config.setString("User", "Nickname", "kept");
	s.config.setString("User", "Description", "also kept");
	EXO_ASSERT(readFile(s.configPath).find("kept") == std::string::npos);

	EXO_ASSERT(pump(loop, [&] { return writes > 0; }, 2000));
	EXO_ASSERT(writes == 1);
	EXO_ASSERT(readFile(s.configPath).find("Nickname=kept") != std::string::npos);
	EXO_ASSERT(readFile(s.sessionPath).empty());
	return 1;
});

EXO_TEST(settingskeeper_what_is_unwritten_is_written_at_the_end,
{
	Samurai::EventLoop loop;
	Stores s;
	{
		QuickDC::SettingsKeeper keeper(loop, s.config, s.session, std::chrono::seconds(60));
		s.session.setString("Hubs", "Open", "adc://example:1511");
	}
	EXO_ASSERT(readFile(s.sessionPath).find("adc://example:1511") != std::string::npos);
	return 1;
});

EXO_TEST(settingskeeper_a_write_that_fails_is_reported,
{
	Samurai::EventLoop loop;
	QuickDC::Preferences config("/nonexistent-quickdc-dir/q.conf");
	QuickDC::Preferences session("/nonexistent-quickdc-dir/s.conf");
	QuickDC::SettingsKeeper keeper(loop, config, session, std::chrono::milliseconds(20));

	int failed = 0;
	keeper.written = [&](const QuickDC::Preferences&, bool ok) { if (!ok) failed++; };

	config.setString("User", "Nickname", "nowhere");
	EXO_ASSERT(pump(loop, [&] { return failed > 0; }, 2000));
	return 1;
});
