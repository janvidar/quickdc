/*
 * The socket monitor's runaway guard.
 *
 * Every backend is level triggered, so a descriptor that is ready stays ready
 * until something acts on it. A handler that answers a readiness report without
 * reading, writing or closing therefore gets the same report on the next pass,
 * and on every pass after that: the poll never blocks and the process spins at
 * the speed of the event loop with nothing to show for it. These check that such
 * a descriptor is taken out of the poll, and - the half that is easy to get
 * wrong - that a handler which is merely slow is left alone.
 */

#include <samurai/samurai.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socketmonitor.h>

#include <chrono>
#include <memory>
#include <string.h>

#ifdef SAMURAI_POSIX
#include <sys/socket.h>
#include <unistd.h>
#endif

/*
 * SPIN_LIMIT in socketmonitor.cpp, which is not exported - so this is the figure
 * the tests below assume rather than the one they read. A change there that is
 * not made here shows up as monitor_pauses_a_handler_that_never_reads failing.
 */
#define MONITOR_TEST_SPIN_LIMIT 64

/** Told the socket is readable, and does not read it. */
class MonitorTestDeaf : public Samurai::IO::Net::SocketEventHandler
{
	public:
		int reports = 0;

	protected:
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { reports++; }
};

/** Reads one byte per readiness report: as slow as a reader can be and still be one. */
class MonitorTestTrickle : public Samurai::IO::Net::SocketEventHandler
{
	public:
		int taken = 0;

	protected:
		void EventDataAvailable(const Samurai::IO::Net::Socket* which) override
		{
			char byte = 0;
			Samurai::IO::Net::Socket* sock = const_cast<Samurai::IO::Net::Socket*>(which);
			if (sock->read(&byte, 1) == 1) taken++;
		}
};

/*
 * A connected pair, one end wrapped in a monitored Socket.
 *
 * A socketpair rather than a loopback connection: what is under test is what the
 * monitor does with a readable descriptor, and this reaches that state without
 * a listener, an accept and a connect timeout to wait for.
 */
struct MonitorTestPair
{
	std::shared_ptr<Samurai::IO::Net::Socket> sock;
	socket_t writer = INVALID_SOCKET;
	bool ok = false;

	MonitorTestPair(Samurai::IO::Net::SocketEventHandler* handler)
	{
#ifdef SAMURAI_POSIX
		socket_t pair[2];
		if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return;

		writer = pair[1];

		const Samurai::IO::Net::InetAddress loopback("127.0.0.1");
		const Samurai::IO::Net::InetSocketAddress addr(loopback, 0);

		sock = Samurai::IO::Net::Socket::create(pair[0], addr);
		if (!sock) { ::close(pair[0]); return; }

		sock->setNonBlocking(true);

		/* Registers the socket for reading, which is what puts it in the poll. */
		sock->setEventHandler(handler);
		ok = true;
#endif
	}

	~MonitorTestPair()
	{
#ifdef SAMURAI_POSIX
		sock.reset();
		if (writer != INVALID_SOCKET) ::close(writer);
#endif
	}

	bool send(size_t bytes)
	{
#ifdef SAMURAI_POSIX
		char block[4096];
		if (bytes > sizeof(block)) return false;
		memset(block, 'x', bytes);
		return ::send(writer, block, bytes, 0) == (ssize_t) bytes;
#else
		(void) bytes;
		return false;
#endif
	}
};

EXO_TEST(monitor_pauses_a_handler_that_never_reads, {
	MonitorTestDeaf deaf;
	MonitorTestPair pair(&deaf);
	if (!pair.ok) return false;
	if (!pair.send(1)) return false;

	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();

	/*
	 * One pass past the limit. Every one of them is answered immediately, since
	 * the byte sent above is still there - which is the condition being tested,
	 * not a side effect of it.
	 */
	int blocked = 0;
	for (int pass = 0; pass < MONITOR_TEST_SPIN_LIMIT + 1; pass++)
	{
		const std::chrono::steady_clock::time_point before =
			std::chrono::steady_clock::now();

		monitor->wait(200);

		if (std::chrono::steady_clock::now() - before > std::chrono::milliseconds(100))
			blocked++;
	}

	/* The pass after the limit is the first one that had nothing to report. */
	if (blocked != 1) return false;

	/* Reported, not swallowed: the handler saw every report up to the limit. */
	if (deaf.reports != MONITOR_TEST_SPIN_LIMIT) return false;

	/* And nothing was read or closed on its behalf. */
	char peeked = 0;
	return ::recv(pair.sock->getFD(), &peeked, 1, MSG_PEEK) == 1;
});

EXO_TEST(monitor_leaves_a_slow_handler_alone, {
	MonitorTestTrickle trickle;
	MonitorTestPair pair(&trickle);
	if (!pair.ok) return false;

	/* Several times the limit, so a guard that counted passes rather than
	   progress would have paused this long before the last byte. */
	const size_t total = MONITOR_TEST_SPIN_LIMIT * 8;
	if (!pair.send(total)) return false;

	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();

	for (size_t pass = 0; pass < total; pass++)
		monitor->wait(200);

	return trickle.taken == (int) total;
});

EXO_TEST(a_socket_the_peer_closed_releases_its_descriptor, {
	/*
	 * Whoever else is holding the socket decides when the object goes; the
	 * descriptor is not theirs to sit on. Only ~Socket used to release it, so a
	 * socket held by a session that was itself waiting to be deleted - which is
	 * every session, since deletion is deferred through the message handler -
	 * kept one open for the life of the process. A client retrying a hub it
	 * cannot reach did that once a minute.
	 */
	MonitorTestDeaf deaf;
	MonitorTestPair pair(&deaf);
	if (!pair.ok) return false;

	const socket_t was = pair.sock->getFD();
	if (was == INVALID_SOCKET) return false;

	/* End of stream, which is what the monitor reports as a close. */
	::close(pair.writer);
	pair.writer = INVALID_SOCKET;

	Samurai::IO::Net::SocketMonitor::getInstance()->wait(0);

	/* Still held here, and still out of the poll and out of descriptors. */
	if (pair.sock->getFD() != INVALID_SOCKET) return false;
	return !pair.sock->isMonitored();
});

EXO_TEST(monitor_forgets_a_descriptor_that_was_closed, {
	MonitorTestDeaf deaf;

	{
		MonitorTestPair pair(&deaf);
		if (!pair.ok) return false;
		if (!pair.send(1)) return false;

		Samurai::IO::Net::SocketMonitor::getInstance()->wait(0);
		if (deaf.reports != 1) return false;
	}

	/*
	 * The socket is gone and its descriptor with it. Nothing may be dispatched
	 * for it afterwards - including by the guard, which holds a weak reference to
	 * a socket it has taken out of the poll and has to notice that it expired
	 * rather than putting it back.
	 */
	for (int pass = 0; pass < 4; pass++)
		Samurai::IO::Net::SocketMonitor::getInstance()->wait(0);

	return deaf.reports == 1;
});
