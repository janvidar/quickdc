/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The inbox a core is handed work through, and the loop that drains it.
 *
 * What matters is what a front end relies on: work posted from another thread
 * runs promptly and in order on the core's thread, a stop is honoured from any
 * thread, and nothing posted after the core has stopped is run - or lost
 * silently, since post() says so.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/inbox.h"

#include <samurai/io/file.h>
#include <samurai/io/net/socketmonitor.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_inbox_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/* A core over scratch files, removed again when this goes. */
struct ScratchCore {
	std::string cfg = scratch("config.conf");
	std::string ses = scratch("session.conf");
	QuickDC::Core* core;

	ScratchCore() : core(new QuickDC::Core(cfg, ses)) { }

	~ScratchCore()
	{
		delete core;
		Samurai::IO::File(cfg).remove();
		Samurai::IO::File(ses).remove();
	}
};

}

EXO_TEST(inbox_a_closed_inbox_refuses_work,
{
	QuickDC::Inbox inbox;
	bool ran = false;

	EXO_ASSERT(!inbox.post([&] { ran = true; }));
	EXO_ASSERT_EQ_UINT(inbox.drain(), 0u);
	EXO_ASSERT(!ran);
	return 1;
});

EXO_TEST(inbox_jobs_run_in_order_when_drained,
{
	QuickDC::Inbox inbox;
	inbox.open();

	std::vector<int> order;
	EXO_ASSERT(inbox.post([&] { order.push_back(1); }));
	EXO_ASSERT(inbox.post([&] { order.push_back(2); }));
	EXO_ASSERT(order.empty());

	EXO_ASSERT_EQ_UINT(inbox.drain(), 2u);
	EXO_ASSERT(order == std::vector<int>({1, 2}));
	inbox.close();
	return 1;
});

/* Posted from inside a job: next drain, so a job that re-posts cannot starve the loop. */
EXO_TEST(inbox_a_job_posted_by_a_job_waits_for_the_next_drain,
{
	QuickDC::Inbox inbox;
	inbox.open();

	int runs = 0;
	EXO_ASSERT(inbox.post([&] { runs++; inbox.post([&] { runs++; }); }));

	EXO_ASSERT_EQ_UINT(inbox.drain(), 1u);
	EXO_ASSERT_EQ_INT(runs, 1);
	EXO_ASSERT_EQ_UINT(inbox.drain(), 1u);
	EXO_ASSERT_EQ_INT(runs, 2);
	inbox.close();
	return 1;
});

/* What was queued before closing still runs; nothing posted after does. */
EXO_TEST(inbox_closing_runs_what_was_queued_and_refuses_what_comes_after,
{
	QuickDC::Inbox inbox;
	inbox.open();

	int runs = 0;
	bool refused = false;
	EXO_ASSERT(inbox.post([&] { runs++; refused = !inbox.post([&] { runs += 100; }); }));

	inbox.close();
	EXO_ASSERT_EQ_INT(runs, 1);
	EXO_ASSERT(refused);
	EXO_ASSERT(!inbox.isOpen());
	EXO_ASSERT(!inbox.post([] { }));
	return 1;
});

/* A post from another thread ends the monitor's wait rather than waiting it out. */
EXO_TEST(inbox_a_post_from_another_thread_interrupts_the_wait,
{
	QuickDC::Inbox inbox;
	if (!inbox.open()) EXO_SKIP("no wake-up could be made");

	std::atomic<bool> ran{false};
	std::thread poster([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		inbox.post([&] { ran = true; });
	});

	const auto began = std::chrono::steady_clock::now();
	while (!ran && std::chrono::steady_clock::now() - began < std::chrono::seconds(5))
	{
		Samurai::IO::Net::SocketMonitor::getInstance()->wait(5000);
		inbox.drain();
	}
	const auto took = std::chrono::steady_clock::now() - began;

	poster.join();
	inbox.close();

	EXO_ASSERT(ran);
	EXO_ASSERT(took < std::chrono::seconds(2));
	return 1;
});

EXO_TEST(inbox_a_core_runs_posted_work_on_its_own_thread_and_stops_when_asked,
{
	ScratchCore scratch;
	QuickDC::Core* core = scratch.core;

	std::atomic<bool> running{false};
	std::thread::id loopThread;
	std::thread::id jobThread;

	EXO_ASSERT(!core->post([] { }));

	std::thread loop([&] {
		loopThread = std::this_thread::get_id();
		core->runUntilStopped([&] { running = true; });
	});

	while (!running) std::this_thread::sleep_for(std::chrono::milliseconds(1));

	std::atomic<bool> done{false};
	EXO_ASSERT(core->post([&] { jobThread = std::this_thread::get_id(); done = true; }));
	while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(1));

	core->requestStop();
	loop.join();

	EXO_ASSERT(jobThread == loopThread);
	EXO_ASSERT(!core->post([] { }));
	return 1;
});

/* A stop asked for before the loop starts is honoured rather than lost. */
EXO_TEST(inbox_a_stop_asked_for_early_is_not_lost,
{
	ScratchCore scratch;
	scratch.core->requestStop();

	bool entered = false;
	scratch.core->runUntilStopped([&] { entered = true; });

	EXO_ASSERT(entered);
	return 1;
});
