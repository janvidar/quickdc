/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * SecurityManager decides whether a peer that turned up is allowed to take a
 * file from us. A clearance is granted when we ask someone to connect, and it
 * names the protocol, the hub session it was arranged on, the peer's ID and
 * the token that identifies the job.
 *
 * Most cases here pass a null socket, so that the table is what answers rather
 * than anything about where the peer came from. The loopback cases at the end
 * pass a real one, since what they are about is the address.
 */

#include "quickdc.h"
#include "network/securitymanager.h"

#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketmonitor.h>

#include <chrono>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

using Clearance = QuickDC::Clearance;

static QuickDC::Clearance* grant(QuickDC::SecurityManager& mgr,
	const char* cid, const char* token,
	Clearance::ClearanceProtocol protocol = Clearance::ADC,
	Clearance::ClearanceType type = Clearance::AuthorizedUser)
{
	QuickDC::Clearance* c = new QuickDC::Clearance(type, protocol, 0, cid, token);
	mgr.grant(c);
	return c;
}

/* Runs the event loop until the condition holds, or gives up. */
template<typename Fn>
static bool pump(Fn done, int timeout_ms = 2000)
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);

	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		monitor->wait(10);
	}
	return true;
}

/*
 * A connected pair on the loopback, so a case can hand isAuthorized() a socket
 * whose peer really is 127.0.0.1 rather than reason about one.
 */
class LoopbackPair
	: public Samurai::IO::Net::ServerSocketEventHandler
	, public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::Socket> accepted;

		bool connect()
		{
			Samurai::IO::Net::InetSocketAddress any(static_cast<uint16_t>(0));
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			if (!server || server->getFD() == -1) return false;
			if (!server->listen()) return false;

			client = Samurai::IO::Net::Socket::create(this, "127.0.0.1",
				server->getLocalPort());
			if (!client) return false;
			client->connect();

			return pump([&] { return accepted != nullptr; });
		}

		~LoopbackPair() override
		{
			accepted.reset();
			client.reset();
			server.reset();
		}

	protected:
		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override
		{ }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			accepted = socket;
			accepted->setEventHandler(this);
		}

		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { }
		void EventDisconnected(const Samurai::IO::Net::Socket*) override { }

	private:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> client;
};

/* No socket, so the loopback shortcut is not what answers. */
static bool authorized(QuickDC::SecurityManager& mgr, const char* cid, const char* token,
	Clearance::ClearanceProtocol protocol = Clearance::ADC)
{
	return mgr.isAuthorized(protocol, 0, 0, cid, token);
}

int main()
{
	setbuf(stdout, 0);

	/* An empty table authorises nobody. */
	{
		QuickDC::SecurityManager mgr;
		CHECK(!authorized(mgr, "CIDA", "token"));
		printf("  ok an empty table authorises nobody\n");
	}

	/* The peer and the job both have to match. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "token1");

		CHECK(authorized(mgr, "CIDA", "token1"));
		CHECK(!authorized(mgr, "CIDA", "token2"));
		CHECK(!authorized(mgr, "CIDB", "token1"));
		CHECK(!authorized(mgr, "CIDB", "token2"));
		printf("  ok a clearance matches one peer and one job\n");
	}

	/* A clearance is for one protocol. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "token1", Clearance::ADC);

		CHECK(authorized(mgr, "CIDA", "token1", Clearance::ADC));
		CHECK(!authorized(mgr, "CIDA", "token1", Clearance::HTTP));
		printf("  ok a clearance does not carry across protocols\n");
	}

	/* Neither side naming a job is not a match. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "token1");

		CHECK(!authorized(mgr, "CIDA", 0));
		printf("  ok a peer with no token is not authorised\n");
	}

	/* Several clearances coexist, and each matches only its own. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1");
		grant(mgr, "CIDB", "t2");
		grant(mgr, "CIDC", "t3");

		CHECK(authorized(mgr, "CIDA", "t1"));
		CHECK(authorized(mgr, "CIDB", "t2"));
		CHECK(authorized(mgr, "CIDC", "t3"));

		/* Not each other's. */
		CHECK(!authorized(mgr, "CIDA", "t2"));
		CHECK(!authorized(mgr, "CIDC", "t1"));
		printf("  ok clearances do not authorise each other's jobs\n");
	}

	/* Once the connection it was granted for has arrived, it is spent. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1");
		grant(mgr, "CIDB", "t2");

		CHECK(mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
		CHECK(!authorized(mgr, "CIDA", "t1"));

		/* And only that one. */
		CHECK(authorized(mgr, "CIDB", "t2"));
		printf("  ok revoking spends one clearance and leaves the rest\n");
	}

	/* Revoking something that was never granted says so. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1");

		CHECK(!mgr.revoke(Clearance::ADC, 0, "CIDA", "nosuchtoken"));
		CHECK(!mgr.revoke(Clearance::ADC, 0, "CIDNOBODY", "t1"));
		CHECK(authorized(mgr, "CIDA", "t1"));
		printf("  ok revoking what was never granted changes nothing\n");
	}

	/* Revoking twice is not a second success. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1");

		CHECK(mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
		CHECK(!mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
		printf("  ok a spent clearance cannot be spent again\n");
	}

	/* An UnknownUser clearance is the wildcard the local hub grants. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1", Clearance::ADC, Clearance::UnknownUser);

		CHECK(authorized(mgr, "ANYBODY", "anything"));
		printf("  ok an UnknownUser clearance authorises anyone\n");
	}

	{
		QuickDC::SecurityManager mgr;
		CHECK(!mgr.isAuthorized(Clearance::ADC, 0, 0, "CIDA", "t1"));
		printf("  ok without a socket the table is what decides\n");
	}

	/* ---- where the peer came from ---- */

	/*
	 * A peer on the loopback gets no special treatment: it is refused exactly as
	 * one from anywhere else is, because nothing granted it a clearance.
	 *
	 * This used to answer true, under `#if 1` with a FIXME asking whether it
	 * should be limited to debug builds - so it was in release builds too, and any
	 * local account or SSH forward to the peer port could take whatever this
	 * client would upload. It is now behind QUICKDC_ALLOW_LOOPBACK_AUTH, which is
	 * off, and this asserts whichever way the build was configured.
	 */
	{
		LoopbackPair pair;
		if (pair.connect())
		{
			QuickDC::SecurityManager mgr;

			/* The address really is what the shortcut used to test. */
			const Samurai::IO::Net::InetAddress* addr = pair.accepted->getAddress();
			CHECK(addr != 0);
			CHECK(addr && addr->isLoopback());

			const bool answer = mgr.isAuthorized(Clearance::ADC, 0,
				pair.accepted.get(), "CIDA", "t1");
#ifdef QUICKDC_ALLOW_LOOPBACK_AUTH
			CHECK(answer);
			printf("  ok a loopback peer is authorised, as this build asked for\n");
#else
			CHECK(!answer);
			printf("  ok a loopback peer with no clearance is refused\n");
#endif
		}
		else
		{
			printf("FAIL could not set up a loopback pair\n");
			failures++;
		}
	}

	/*
	 * And a clearance still authorises one, so the gate did not simply refuse
	 * everything that arrives with a socket attached.
	 */
	{
		LoopbackPair pair;
		if (pair.connect())
		{
			QuickDC::SecurityManager mgr;
			grant(mgr, "CIDA", "t1");

			CHECK(mgr.isAuthorized(Clearance::ADC, 0, pair.accepted.get(),
				"CIDA", "t1"));

			const bool wrong = mgr.isAuthorized(Clearance::ADC, 0,
				pair.accepted.get(), "CIDA", "wrong-token");
#ifdef QUICKDC_ALLOW_LOOPBACK_AUTH
			/*
			 * Asserted rather than tolerated, because it is the whole reason the
			 * shortcut is not on by default: the address is answered before the
			 * token is looked at, so a peer on the loopback does not need to know
			 * the token at all.
			 */
			CHECK(wrong);
			printf("  ok a wrong token is authorised over loopback too, which is "
				"what makes this build insecure\n");
#else
			CHECK(!wrong);
			printf("  ok a cleared loopback peer is authorised and a wrong token is not\n");
#endif
		}
		else failures++;
	}

	/*
	 * A socket with no peer answers null for its address, which the shortcut
	 * dereferenced without checking.
	 */
	{
		QuickDC::SecurityManager mgr;
		std::shared_ptr<Samurai::IO::Net::Socket> lonely =
			Samurai::IO::Net::Socket::create(
				static_cast<Samurai::IO::Net::SocketEventHandler*>(0),
				std::string("127.0.0.1"), static_cast<uint16_t>(1));

		if (lonely)
		{
			CHECK(lonely->getAddress() == 0);
			CHECK(!mgr.isAuthorized(Clearance::ADC, 0, lonely.get(), "CIDA", "t1"));
			printf("  ok a socket that never connected does not crash the check\n");
		}
		else failures++;
	}

	/*
	 * NMDC matches on the nickname alone. The protocol has no token to name a
	 * connection request by, so the branch that used to be compiled out took
	 * every NMDC peer as unauthorised - and nothing granted an NMDC clearance
	 * either, so gating an upload on this would have refused all of them.
	 */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "alice", 0, Clearance::NMDC);

		CHECK(authorized(mgr, "alice", 0, Clearance::NMDC));
		printf("  ok an NMDC clearance authorises the nick it names\n");
	}

	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "alice", 0, Clearance::NMDC);

		CHECK(!authorized(mgr, "bob", 0, Clearance::NMDC));
		printf("  ok an NMDC clearance does not authorise a different nick\n");
	}

	/* A nick we never asked to connect has nothing granted for it at all. */
	{
		QuickDC::SecurityManager mgr;
		CHECK(!authorized(mgr, "stranger", 0, Clearance::NMDC));
		printf("  ok an unexpected NMDC peer is not authorised\n");
	}

	/* The protocols do not authorise each other. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "alice", 0, Clearance::NMDC);

		CHECK(!authorized(mgr, "alice", "t1", Clearance::ADC));
		printf("  ok an NMDC clearance does not authorise an ADC request\n");
	}

	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "CIDA", "t1", Clearance::ADC);

		CHECK(!authorized(mgr, "CIDA", 0, Clearance::NMDC));
		printf("  ok an ADC clearance does not authorise an NMDC request\n");
	}

	/* A null ID is refused rather than reaching strcmp. */
	{
		QuickDC::SecurityManager mgr;
		grant(mgr, "alice", 0, Clearance::NMDC);

		CHECK(!authorized(mgr, 0, 0, Clearance::NMDC));
		printf("  ok an NMDC request with no nick is refused\n");
	}

	{
		QuickDC::SecurityManager mgr;
		grant(mgr, 0, "t1", Clearance::ADC);

		CHECK(!authorized(mgr, 0, "t1", Clearance::ADC));
		printf("  ok an ADC clearance with no ID does not match a null ID\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
