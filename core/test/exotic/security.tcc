/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
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
#include <samurai/io/net/tlsfactory.h>
#include <samurai/io/file.h>

#include "api/core.h"

#include <chrono>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

using Clearance = QuickDC::Clearance;

QuickDC::Clearance* grant(QuickDC::SecurityManager& mgr,
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
bool pump(Fn done, int timeout_ms = 2000)
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

/* No socket, so the loopback shortcut is not what answers. */
bool authorized(QuickDC::SecurityManager& mgr, const char* cid, const char* token,
	Clearance::ClearanceProtocol protocol = Clearance::ADC)
{
	return mgr.isAuthorized(protocol, 0, 0, cid, token);
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

}

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_security_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

}

/* ------------------------------------------------------------------------- */
/* The clearance table                                                       */
/* ------------------------------------------------------------------------- */

EXO_TEST(security_an_empty_table_authorises_nobody,
{
	QuickDC::SecurityManager mgr;
	EXO_ASSERT(!authorized(mgr, "CIDA", "token"));
	return 1;
});

EXO_TEST(security_a_clearance_matches_one_peer_and_one_job,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "token1");

	EXO_ASSERT(authorized(mgr, "CIDA", "token1"));
	EXO_ASSERT(!authorized(mgr, "CIDA", "token2"));
	EXO_ASSERT(!authorized(mgr, "CIDB", "token1"));
	EXO_ASSERT(!authorized(mgr, "CIDB", "token2"));
	return 1;
});

EXO_TEST(security_a_clearance_does_not_carry_across_protocols,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "token1", Clearance::ADC);

	EXO_ASSERT(authorized(mgr, "CIDA", "token1", Clearance::ADC));
	EXO_ASSERT(!authorized(mgr, "CIDA", "token1", Clearance::HTTP));
	return 1;
});

EXO_TEST(security_a_peer_with_no_token_is_not_authorised,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "token1");

	EXO_ASSERT(!authorized(mgr, "CIDA", 0));
	return 1;
});

EXO_TEST(security_clearances_do_not_authorise_each_others_jobs,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "t1");
	grant(mgr, "CIDB", "t2");
	grant(mgr, "CIDC", "t3");

	EXO_ASSERT(authorized(mgr, "CIDA", "t1"));
	EXO_ASSERT(authorized(mgr, "CIDB", "t2"));
	EXO_ASSERT(authorized(mgr, "CIDC", "t3"));

	/* Not each other's. */
	EXO_ASSERT(!authorized(mgr, "CIDA", "t2"));
	EXO_ASSERT(!authorized(mgr, "CIDC", "t1"));
	return 1;
});

/* Once the connection it was granted for has arrived, it is spent. */
EXO_TEST(security_revoking_spends_one_clearance_and_leaves_the_rest,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "t1");
	grant(mgr, "CIDB", "t2");

	EXO_ASSERT(mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
	EXO_ASSERT(!authorized(mgr, "CIDA", "t1"));

	/* And only that one. */
	EXO_ASSERT(authorized(mgr, "CIDB", "t2"));
	return 1;
});

EXO_TEST(security_revoking_what_was_never_granted_changes_nothing,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "t1");

	EXO_ASSERT(!mgr.revoke(Clearance::ADC, 0, "CIDA", "nosuchtoken"));
	EXO_ASSERT(!mgr.revoke(Clearance::ADC, 0, "CIDNOBODY", "t1"));
	EXO_ASSERT(authorized(mgr, "CIDA", "t1"));
	return 1;
});

EXO_TEST(security_a_spent_clearance_cannot_be_spent_again,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "t1");

	EXO_ASSERT(mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
	EXO_ASSERT(!mgr.revoke(Clearance::ADC, 0, "CIDA", "t1"));
	return 1;
});

/* An UnknownUser clearance is the wildcard the local hub grants. */
EXO_TEST(security_an_unknownuser_clearance_authorises_anyone,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "t1", Clearance::ADC, Clearance::UnknownUser);

	EXO_ASSERT(authorized(mgr, "ANYBODY", "anything"));
	return 1;
});

EXO_TEST(security_without_a_socket_the_table_is_what_decides,
{
	QuickDC::SecurityManager mgr;
	EXO_ASSERT(!mgr.isAuthorized(Clearance::ADC, 0, 0, "CIDA", "t1"));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Where the peer came from                                                  */
/* ------------------------------------------------------------------------- */

/*
 * A peer on the loopback gets no special treatment: it is refused exactly as
 * one from anywhere else is, because nothing granted it a clearance.
 *
 * This used to answer true, under `#if 1` with a FIXME asking whether it should
 * be limited to debug builds - so it was in release builds too, and any local
 * account or SSH forward to the peer port could take whatever this client would
 * upload. It is now behind QUICKDC_ALLOW_LOOPBACK_AUTH, which is off, and this
 * asserts whichever way the build was configured.
 */
EXO_TEST(security_a_loopback_peer_with_no_clearance_is_refused,
{
	LoopbackPair pair;
	if (!pair.connect()) EXO_SKIP("could not set up a loopback pair");

	QuickDC::SecurityManager mgr;

	/* The address really is what the shortcut used to test. */
	const Samurai::IO::Net::InetAddress* addr = pair.accepted->getAddress();
	EXO_ASSERT_NOT_NULL(addr);
	EXO_ASSERT(addr->isLoopback());

	const bool answer = mgr.isAuthorized(Clearance::ADC, 0,
		pair.accepted.get(), "CIDA", "t1");
#ifdef QUICKDC_ALLOW_LOOPBACK_AUTH
	EXO_ASSERT(answer);
#else
	EXO_ASSERT(!answer);
#endif
	return 1;
});

/*
 * And a clearance still authorises one, so the gate did not simply refuse
 * everything that arrives with a socket attached.
 */
EXO_TEST(security_a_cleared_loopback_peer_is_authorised,
{
	LoopbackPair pair;
	if (!pair.connect()) EXO_SKIP("could not set up a loopback pair");

	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "t1");

	EXO_ASSERT(mgr.isAuthorized(Clearance::ADC, 0, pair.accepted.get(),
		"CIDA", "t1"));

	const bool wrong = mgr.isAuthorized(Clearance::ADC, 0,
		pair.accepted.get(), "CIDA", "wrong-token");
#ifdef QUICKDC_ALLOW_LOOPBACK_AUTH
	/*
	 * Asserted rather than tolerated, because it is the whole reason the
	 * shortcut is not on by default: the address is answered before the token is
	 * looked at, so a peer on the loopback does not need to know the token at
	 * all.
	 */
	EXO_ASSERT(wrong);
#else
	EXO_ASSERT(!wrong);
#endif
	return 1;
});

/*
 * A socket with no peer answers null for its address, which the shortcut
 * dereferenced without checking.
 */
EXO_TEST(security_a_socket_that_never_connected_does_not_crash_the_check,
{
	QuickDC::SecurityManager mgr;
	std::shared_ptr<Samurai::IO::Net::Socket> lonely =
		Samurai::IO::Net::Socket::create(
			static_cast<Samurai::IO::Net::SocketEventHandler*>(0),
			std::string("127.0.0.1"), static_cast<uint16_t>(1));

	if (!lonely) EXO_SKIP("could not create a socket");

	EXO_ASSERT_NULL(lonely->getAddress());
	EXO_ASSERT(!mgr.isAuthorized(Clearance::ADC, 0, lonely.get(), "CIDA", "t1"));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* NMDC, which names a peer by nickname and carries no token                 */
/* ------------------------------------------------------------------------- */

/*
 * The branch that used to be compiled out took every NMDC peer as unauthorised -
 * and nothing granted an NMDC clearance either, so gating an upload on this
 * would have refused all of them.
 */
EXO_TEST(security_an_nmdc_clearance_authorises_the_nick_it_names,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "alice", 0, Clearance::NMDC);

	EXO_ASSERT(authorized(mgr, "alice", 0, Clearance::NMDC));
	return 1;
});

EXO_TEST(security_an_nmdc_clearance_does_not_authorise_a_different_nick,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "alice", 0, Clearance::NMDC);

	EXO_ASSERT(!authorized(mgr, "bob", 0, Clearance::NMDC));
	return 1;
});

/* A nick we never asked to connect has nothing granted for it at all. */
EXO_TEST(security_an_unexpected_nmdc_peer_is_not_authorised,
{
	QuickDC::SecurityManager mgr;
	EXO_ASSERT(!authorized(mgr, "stranger", 0, Clearance::NMDC));
	return 1;
});

/* The protocols do not authorise each other. */
EXO_TEST(security_an_nmdc_clearance_does_not_authorise_an_adc_request,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "alice", 0, Clearance::NMDC);

	EXO_ASSERT(!authorized(mgr, "alice", "t1", Clearance::ADC));
	return 1;
});

EXO_TEST(security_an_adc_clearance_does_not_authorise_an_nmdc_request,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "CIDA", "t1", Clearance::ADC);

	EXO_ASSERT(!authorized(mgr, "CIDA", 0, Clearance::NMDC));
	return 1;
});

/* A null ID is refused rather than reaching strcmp. */
EXO_TEST(security_an_nmdc_request_with_no_nick_is_refused,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, "alice", 0, Clearance::NMDC);

	EXO_ASSERT(!authorized(mgr, 0, 0, Clearance::NMDC));
	return 1;
});

EXO_TEST(security_an_adc_clearance_with_no_id_does_not_match_a_null_id,
{
	QuickDC::SecurityManager mgr;
	grant(mgr, 0, "t1", Clearance::ADC);

	EXO_ASSERT(!authorized(mgr, 0, "t1", Clearance::ADC));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The certificate this client presents                                      */
/* ------------------------------------------------------------------------- */

/*
 * A peer transfer has to be encrypted unless the user says otherwise, and a peer
 * dialling us can only be answered with TLS if there is a certificate to present
 * - so Core generates one on a client that has none. Samurai's own suite covers
 * the generator; what is asserted here is the part that is ours to get wrong.
 *
 * The name is refused rather than truncated if it is not one a certificate can
 * carry, and a refused name means no certificate, which means no encrypted
 * transfer and nothing on the wire to say why. This is the case that fails if
 * QUICKDC_CERTIFICATE_NAME is changed to something the generator will not take.
 */
EXO_TEST(security_the_name_this_client_asserts_is_one_that_can_be_generated,
{
	const std::string key = scratch("cert.key");
	const std::string crt = scratch("cert.crt");

	const bool made = Samurai::IO::Net::TlsFactory::generateSelfSignedCertificate(
		key.c_str(), crt.c_str(), QUICKDC_CERTIFICATE_NAME);

	const bool key_there = Samurai::IO::File(key).exists();
	const bool crt_there = Samurai::IO::File(crt).exists();

	Samurai::IO::File(key).remove();
	Samurai::IO::File(crt).remove();

	EXO_ASSERT(made);
	EXO_ASSERT(key_there);
	EXO_ASSERT(crt_there);
	return 1;
});

/*
 * And the two paths are ones the generator can resolve, which is why they are
 * the same strings setKeys() is given: a key written beside a literal '~' would
 * not be the key the process then reads.
 */
EXO_TEST(security_the_certificate_paths_are_under_the_client_directory,
{
	const std::string key = QUICKDC_PRIVATE_KEY_FILE;
	const std::string crt = QUICKDC_CERTIFICATE_FILE;

	EXO_ASSERT(key.compare(0, 15, "~/.quickdc/ssl/") == 0);
	EXO_ASSERT(crt.compare(0, 15, "~/.quickdc/ssl/") == 0);
	EXO_ASSERT(key != crt);

	/* Resolved to somewhere absolute, and not to a directory called '~'. */
	const std::string resolved = Samurai::IO::File::resolvePath(key);
	EXO_ASSERT(!resolved.empty());
	EXO_ASSERT(resolved[0] == '/');
	EXO_ASSERT(resolved.find("/~/") == std::string::npos);
	return 1;
});
