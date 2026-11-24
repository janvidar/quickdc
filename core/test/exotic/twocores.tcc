/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Two cores in one process, each on its own event loop.
 *
 * Turning one core's loop must dispatch nothing of the other's - no socket, no
 * timer, no message - and either may go first. Both are the hub-only core over
 * scratch files, so nothing here reads or writes the user's own configuration.
 */

#include "quickdc.h"
#include "api/core.h"

#include <samurai/eventloop.h>
#include <samurai/io/file.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/messagehandler.h>
#include <samurai/timer.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_twocores_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

struct ScratchCore {
	std::string cfg = scratch("config.conf");
	std::string ses = scratch("session.conf");
	std::unique_ptr<QuickDC::Core> core = std::make_unique<QuickDC::Core>(cfg, ses);

	~ScratchCore()
	{
		core.reset();
		Samurai::IO::File(cfg).remove();
		Samurai::IO::File(ses).remove();
	}
};

/* A listening socket on one loop and a client dialling it from the same loop. */
class Loopback
	: public Samurai::IO::Net::ServerSocketEventHandler
	, public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::Socket> accepted;

		bool dial(Samurai::EventLoop& loop)
		{
			const Samurai::IO::Net::InetAddress loopback("127.0.0.1");
			const Samurai::IO::Net::InetSocketAddress any(loopback, 0);
			server = Samurai::IO::Net::ServerSocket::create(loop, this, any);
			if (!server || server->getFD() == -1 || !server->listen()) return false;

			client = Samurai::IO::Net::Socket::create(loop, this, "127.0.0.1",
				server->getLocalPort());
			if (!client) return false;
			client->connect();
			return true;
		}

		~Loopback() override
		{
			accepted.reset();
			client.reset();
			server.reset();
		}

	protected:
		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			accepted = std::move(socket);
			accepted->setEventHandler(this);
		}

		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { }

	private:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> client;
};

class Counting : public Samurai::TimerListener
{
	public:
		int fired = 0;
		void EventTimeout(Samurai::Timer*) override { fired++; }
};

/* An id no other listener in the core answers to. */
const size_t MSG_TWOCORES_PING = 0x7c0de;

class Ping : public Samurai::MessageListener
{
	public:
		explicit Ping(Samurai::EventLoop& loop) : Samurai::MessageListener(loop) { }

		int received = 0;

		bool EventMessage(const Samurai::Message* msg) override
		{
			if (msg->getID() != MSG_TWOCORES_PING) return false;
			received++;
			return true;
		}
};

/* Turn 'core' until the condition holds or two seconds have gone by. */
template<typename Fn>
bool runUntil(QuickDC::Core& core, Fn done)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		core.run();
	}
	return true;
}

}

EXO_TEST(twocores_each_has_a_loop_of_its_own,
{
	ScratchCore a;
	ScratchCore b;

	EXO_ASSERT(&a.core->loop() != &b.core->loop());
	EXO_ASSERT(&a.core->loop().monitor() != &b.core->loop().monitor());
	EXO_ASSERT(&a.core->loop().timers() != &b.core->loop().timers());
	EXO_ASSERT(&a.core->loop().messages() != &b.core->loop().messages());
	return 1;
});

/* The kernel completes both connections; only the loop turned is told. */
EXO_TEST(twocores_running_one_does_not_dispatch_the_others_sockets,
{
	ScratchCore a;
	ScratchCore b;

	Loopback onA;
	Loopback onB;
	if (!onA.dial(a.core->loop()) || !onB.dial(b.core->loop()))
		EXO_SKIP("could not listen on a loopback port");

	EXO_ASSERT(runUntil(*a.core, [&] { return onA.accepted != nullptr; }));
	EXO_ASSERT(onA.accepted->getLoop() == &a.core->loop());

	/* Long enough for B's connection to have been waiting to be accepted. */
	for (int turn = 0; turn < 3; turn++) a.core->run();
	EXO_ASSERT(onB.accepted == nullptr);

	EXO_ASSERT(runUntil(*b.core, [&] { return onB.accepted != nullptr; }));
	EXO_ASSERT(onB.accepted->getLoop() == &b.core->loop());
	return 1;
});

EXO_TEST(twocores_running_one_does_not_fire_the_others_timers,
{
	ScratchCore a;
	ScratchCore b;

	Counting onA;
	Counting onB;
	Samurai::Timer timerA(a.core->loop(), &onA, std::chrono::milliseconds(1), true);
	Samurai::Timer timerB(b.core->loop(), &onB, std::chrono::milliseconds(1), true);

	EXO_ASSERT(runUntil(*a.core, [&] { return onA.fired == 1; }));
	a.core->run();
	EXO_ASSERT_EQ_INT(onB.fired, 0);

	EXO_ASSERT(runUntil(*b.core, [&] { return onB.fired == 1; }));
	EXO_ASSERT_EQ_INT(onA.fired, 1);
	return 1;
});

EXO_TEST(twocores_a_message_is_delivered_only_on_the_loop_it_was_posted_to,
{
	ScratchCore a;
	ScratchCore b;

	Ping onA(a.core->loop());
	Ping onB(b.core->loop());

	a.core->loop().messages().postMessage(MSG_TWOCORES_PING, nullptr, 0, 0);

	a.core->run();
	b.core->run();
	EXO_ASSERT_EQ_INT(onA.received, 1);
	EXO_ASSERT_EQ_INT(onB.received, 0);

	/* Once each turn, not again on the next. */
	a.core->run();
	EXO_ASSERT_EQ_INT(onA.received, 1);
	return 1;
});

/* Each driven from a thread of its own, at the same time. */
EXO_TEST(twocores_run_side_by_side_on_two_threads,
{
	ScratchCore a;
	ScratchCore b;

	std::atomic<int> started{0};
	std::thread::id threadA;
	std::thread::id threadB;

	std::thread runA([&] {
		threadA = std::this_thread::get_id();
		a.core->runUntilStopped([&] { started++; });
	});
	std::thread runB([&] {
		threadB = std::this_thread::get_id();
		b.core->runUntilStopped([&] { started++; });
	});

	while (started < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));

	std::atomic<bool> doneA{false};
	std::atomic<bool> doneB{false};
	std::thread::id ranA;
	std::thread::id ranB;
	EXO_ASSERT(a.core->post([&] { ranA = std::this_thread::get_id(); doneA = true; }));
	EXO_ASSERT(b.core->post([&] { ranB = std::this_thread::get_id(); doneB = true; }));
	while (!doneA || !doneB) std::this_thread::sleep_for(std::chrono::milliseconds(1));

	a.core->requestStop();
	b.core->requestStop();
	runA.join();
	runB.join();

	EXO_ASSERT(ranA == threadA);
	EXO_ASSERT(ranB == threadB);
	EXO_ASSERT(threadA != threadB);
	return 1;
});

/* Each holding a socket and a timer when it goes, so there is something to tear down. */
static int destroyInOrder(bool firstMadeFirst)
{
	auto a = std::make_unique<ScratchCore>();
	auto b = std::make_unique<ScratchCore>();

	Loopback onA;
	Loopback onB;
	if (!onA.dial(a->core->loop()) || !onB.dial(b->core->loop())) return -1;
	Counting ticks;
	auto timerA = std::make_unique<Samurai::Timer>(a->core->loop(), &ticks, 60, false);
	auto timerB = std::make_unique<Samurai::Timer>(b->core->loop(), &ticks, 60, false);

	if (!runUntil(*a->core, [&] { return onA.accepted != nullptr; })) return 0;
	if (!runUntil(*b->core, [&] { return onB.accepted != nullptr; })) return 0;

	if (firstMadeFirst)
	{
		timerA.reset();
		a.reset();
		b->core->run();
		timerB.reset();
		b.reset();
	}
	else
	{
		timerB.reset();
		b.reset();
		a->core->run();
		timerA.reset();
		a.reset();
	}
	return 1;
}

EXO_TEST(twocores_the_first_made_can_go_first,
{
	const int result = destroyInOrder(true);
	if (result < 0) EXO_SKIP("could not listen on a loopback port");
	return result;
});

EXO_TEST(twocores_the_last_made_can_go_first,
{
	const int result = destroyInOrder(false);
	if (result < 0) EXO_SKIP("could not listen on a loopback port");
	return result;
});
