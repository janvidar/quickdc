/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The command and event boundary a front end drives the core through.
 *
 * Each case runs the core the way a front end does - its loop on a thread of its
 * own, commands submitted from this one, events taken off a sink - because that
 * crossing is what the boundary is for.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/protocol.h"

#include <samurai/io/file.h>

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_service_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/* Every event delivered, copied off the core's thread. */
class Recorder : public QuickDC::Api::EventSink {
	public:
		void deliver(const QuickDC::Api::Event& event) override
		{
			std::lock_guard<std::mutex> guard(mutex);
			events.push_back(event);
			arrived.notify_all();
		}

		bool waitFor(size_t count, std::chrono::seconds limit = std::chrono::seconds(10))
		{
			std::unique_lock<std::mutex> lock(mutex);
			return arrived.wait_for(lock, limit, [&] { return events.size() >= count; });
		}

		std::vector<QuickDC::Api::Event> taken()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return events;
		}

	private:
		std::mutex mutex;
		std::condition_variable arrived;
		std::vector<QuickDC::Api::Event> events;
};

/* A scratch core whose loop runs on a thread of its own for as long as this lives. */
class Running {
	public:
		Running()
			: core(new QuickDC::Core(cfg, ses))
		{
			std::mutex mutex;
			std::condition_variable started;
			bool up = false;

			loop = std::thread([&] {
				core->runUntilStopped([&] {
					std::lock_guard<std::mutex> guard(mutex);
					up = true;
					started.notify_all();
				});
			});

			std::unique_lock<std::mutex> lock(mutex);
			started.wait(lock, [&] { return up; });
		}

		~Running()
		{
			core->requestStop();
			loop.join();
			delete core;
			Samurai::IO::File(cfg).remove();
			Samurai::IO::File(ses).remove();
		}

		/* Run 'job' on the core's thread and wait for it. */
		template<typename Job>
		void onCore(Job job)
		{
			std::mutex mutex;
			std::condition_variable done;
			bool finished = false;

			core->post([&] {
				job();
				std::lock_guard<std::mutex> guard(mutex);
				finished = true;
				done.notify_all();
			});

			std::unique_lock<std::mutex> lock(mutex);
			done.wait(lock, [&] { return finished; });
		}

		std::string cfg = scratch("config.conf");
		std::string ses = scratch("session.conf");
		QuickDC::Core* core;

	private:
		std::thread loop;
};

}

EXO_TEST(service_a_command_is_refused_while_the_core_is_not_running,
{
	const std::string cfg = scratch("config.conf");
	const std::string ses = scratch("session.conf");
	{
		QuickDC::Core core(cfg, ses);
		EXO_ASSERT(!core.submit(QuickDC::Api::Commands::FetchHubList{"http://127.0.0.1:1/"}));
	}
	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();
	return 1;
});

EXO_TEST(service_a_hublist_nobody_serves_is_reported_as_failed,
{
	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	const std::string url = "http://127.0.0.1:1/hublist.xml";
	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::FetchHubList{url}));
	EXO_ASSERT(recorder.waitFor(1));

	const std::vector<QuickDC::Api::Event> events = recorder.taken();
	const auto* failed = events[0].as<QuickDC::Api::Events::HubListFailed>();
	EXO_ASSERT(failed != nullptr);
	if (failed)
	{
		EXO_ASSERT(failed->url == url);
		EXO_ASSERT(!failed->reason.empty());
	}

	running.onCore([&] { subscription.reset(); });
	return 1;
});

EXO_TEST(service_an_address_that_is_not_one_is_reported_as_failed,
{
	Running running;
	Recorder recorder;
	QuickDC::Subscription subscription;
	running.onCore([&] { subscription = running.core->subscribe(&recorder); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::FetchHubList{"not an address"}));
	EXO_ASSERT(recorder.waitFor(1));

	const std::vector<QuickDC::Api::Event> events = recorder.taken();
	EXO_ASSERT(events[0].as<QuickDC::Api::Events::HubListFailed>() != nullptr);

	running.onCore([&] { subscription.reset(); });
	return 1;
});

/* A sink whose subscription ended hears nothing more, and every other one still does. */
EXO_TEST(service_an_ended_subscription_is_told_nothing_more,
{
	Running running;
	Recorder gone;
	Recorder staying;
	QuickDC::Subscription a;
	QuickDC::Subscription b;
	running.onCore([&] {
		a = running.core->subscribe(&gone);
		b = running.core->subscribe(&staying);
	});

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::FetchHubList{"not an address"}));
	EXO_ASSERT(staying.waitFor(1) && gone.waitFor(1));

	running.onCore([&] { a.reset(); });

	EXO_ASSERT(running.core->submit(QuickDC::Api::Commands::FetchHubList{"still not one"}));
	EXO_ASSERT(staying.waitFor(2));
	EXO_ASSERT_EQ_UINT(gone.taken().size(), 1u);

	running.onCore([&] { b.reset(); });
	return 1;
});
