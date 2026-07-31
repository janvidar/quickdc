/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * A real DC::ClientSession against a peer that only exists for the test - the
 * NMDC counterpart of clientsession.cpp.
 *
 * This one needs more scaffolding than the ADC peer did. An NMDC peer
 * connection is answered with a challenge carrying our own nickname, and the
 * session takes that from the hub session's local user, so there has to be a
 * hub to be logged in to first. Each case therefore stands up a hub, logs a
 * session in to it, and only then makes the peer connection.
 *
 * As in the ADC harness, the pair is made over the loopback interface, where
 * SecurityManager::isAuthorized() answers true for anyone before it consults
 * its table. security.cpp covers the clearance matching itself.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/hub.h"
#include "network/dc/hubsession.h"
#include "network/dc/clientsession.h"
#include "network/user.h"
#include "network/usermanager.h"

#include <samurai/io/file.h>
#include <samurai/io/net/url.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>

#include <chrono>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const int TIMEOUT_MS = 5000;

/* Long enough for a key to be derived from, as a real peer sends. */
#define PEER_LOCK "EXTENDEDPROTOCOLABCABCABCABCABCABC"

template<typename Fn>
static bool pump(Fn done, int timeout_ms = TIMEOUT_MS)
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

/* Enough of a listener to construct a hub with. */
class Listener : public QuickDC::HubListener
{
	public:
		void EventNetStatus(enum StatusNetwork) override { }
		void EventHubStatus(enum StatusHub) override { }
		void EventSystemError(const char*) override { }
		void EventChat(const QuickDC::User*, const char*, bool, bool) override { }
		void EventPrivateChat(const QuickDC::User*, const QuickDC::User*,
			const char*, const char*, bool, bool) override { }
		void EventHubMessage(const char*, const char*) override { }
		void EventUserJoin(const char*) override { }
		void EventUserLeave(const QuickDC::User*, const char*, bool) override { }
		void EventUsersCleanup() override { }
		void EventUserUpdate(const QuickDC::User*) override { }
		void EventSearchResult(void*) override { }
		void EventHubName(const char*) override { }
		bool EventHubRedirect(const char*, uint16_t) override { return false; }
		void EventHubAutenticate() override { }
		bool EventClientConnect(const char*, uint16_t, const QuickDC::User*) override
		{ return false; }
		bool EventClientConnect(const QuickDC::User*) override { return false; }
};

/*
 * One end of a loopback connection, kept as a stream so it can be read a
 * command at a time. Used for both the hub and the peer.
 */
class Endpoint :
	public Samurai::IO::Net::ServerSocketEventHandler,
	public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> mine;
		std::shared_ptr<Samurai::IO::Net::Socket> theirs;

		/*
		 * Whichever of the two above is this end. As the hub we accept, so it
		 * is the accepted one; as the peer we dial, so it is the dialling one.
		 */
		std::shared_ptr<Samurai::IO::Net::Socket> local;

		std::string received;
		bool connected = false;
		bool adopt = true;   /* false leaves 'theirs' for someone else to take */

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			return server && server->getFD() != -1 && server->listen();
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }

		/* Dials ourselves, so both ends are here. */
		bool dial()
		{
			if (!listen()) return false;
			mine = Samurai::IO::Net::Socket::create(this,
				std::string("127.0.0.1"), port());
			if (!mine) return false;
			mine->connect();
			if (!pump([&] { return connected && theirs != 0; })) return false;

			local = mine;
			return true;
		}

		/* We are the one accepting, so the accepted socket is our end. */
		bool accepted()
		{
			if (!pump([&] { return theirs != 0; })) return false;

			local = theirs;
			return true;
		}

		/* NMDC ends every command with '|'. */
		void send(const std::string& command)
		{
			if (!local) return;
			const std::string out = command + "|";
			local->write(out.data(), out.size());
		}

		bool nextCommand(std::string& out)
		{
			const bool got = pump([&] {
				drain();
				return received.find('|') != std::string::npos;
			});

			if (!got) return false;

			const size_t at = received.find('|');
			out = received.substr(0, at);
			received.erase(0, at + 1);
			return true;
		}

		bool expect(const char* prefix, std::string& out)
		{
			for (int n = 0; n < 30; n++)
			{
				if (!nextCommand(out)) return false;
				if (out.compare(0, strlen(prefix), prefix) == 0) return true;
			}
			return false;
		}

		void drain()
		{
			if (!local) return;
			for (;;)
			{
				char buf[4096];
				size_t got = 0;
				std::error_code ec;
				if (local->read(buf, sizeof(buf), got, ec) != Samurai::IO::ReadResult::Ok) break;
				if (!got) break;
				received.append(buf, got);
			}
		}

		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			theirs = socket;
			if (adopt) theirs->setEventHandler(this);
		}

		void EventHostLookup(const Samurai::IO::Net::Socket*) override { }
		void EventHostFound(const Samurai::IO::Net::Socket*) override { }
		void EventConnecting(const Samurai::IO::Net::Socket*) override { }
		void EventConnected(const Samurai::IO::Net::Socket*) override { connected = true; }
		void EventTimeout(const Samurai::IO::Net::Socket*) override { }
		void EventDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { }
		void EventCanWrite(const Samurai::IO::Net::Socket*) override { }
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override { }
};

/*
 * A hub with a session logged in to it, and a peer connection made to that
 * session - which is where every case below starts.
 */
class Fixture
{
	public:
		Endpoint hub;
		Endpoint peer;
		Listener listener;
		std::unique_ptr<Samurai::IO::Net::URL> url;
		std::unique_ptr<QuickDC::Hub> ui;
		DC::HubSession* hubsession = 0;
		DC::ClientSession* session = 0;
		std::string nick;
		bool ready = false;

		Fixture()
		{
			if (!hub.listen()) return;

			const std::string address =
				"dchub://127.0.0.1:" + std::to_string(hub.port());
			url = std::make_unique<Samurai::IO::Net::URL>(address);

			ui = std::make_unique<QuickDC::Hub>(&listener, url.get());
			hubsession = new DC::HubSession(ui.get(), &listener, url.get());
			hubsession->connect();
			if (!hub.accepted()) return;

			if (!login()) return;

			/* The peer's end is ours; the accepted one goes to the session,
			   which adopts it. */
			peer.adopt = false;
			if (!peer.dial()) return;

			session = new DC::ClientSession(peer.theirs, hubsession);
			ready = true;
		}

		~Fixture()
		{
			delete session;
			delete hubsession;
			ui.reset();
		}

		Fixture(const Fixture&) = delete;
		Fixture& operator=(const Fixture&) = delete;

		void sayHub(const std::string& command)
		{
			hub.send(command);
			pump([] { return false; }, 60);
		}

		void say(const std::string& command)
		{
			peer.send(command);
			pump([] { return false; }, 60);
		}

		/* Enough of a hub login that the session has a local user to name. */
		bool login()
		{
			sayHub("$Lock " PEER_LOCK " Pk=TESTHUB");

			std::string line;
			if (!hub.expect("$ValidateNick ", line)) return false;
			nick = line.substr(strlen("$ValidateNick "));
			if (nick.empty()) return false;

			sayHub("$Hello " + nick);
			if (!hub.expect("$MyINFO ", line)) return false;

			sayHub("$MyINFO $ALL " + nick + " $ $DSL\x01$$0$");
			return hubsession->isLoggedIn();
		}

		/* The peer speaks first: its nick, then its lock. */
		bool handshake()
		{
			say("$MyNick peernick");
			say("$Lock " PEER_LOCK " Pk=PEER");

			std::string line;
			return peer.expect("$MyNick ", line) && peer.expect("$Lock ", line);
		}
};

/* ------------------------------------------------------------------------- */

static void run()
{
	{
		Fixture f;
		CHECK(f.ready);
		CHECK(f.hubsession && f.hubsession->isLoggedIn());
		printf("  ok a peer connection is accepted on a logged in session\n");
	}

	/* The peer names itself and challenges; we answer in kind. */
	{
		Fixture f;
		if (f.ready)
		{
			f.say("$MyNick peernick");
			f.say("$Lock " PEER_LOCK " Pk=PEER");

			std::string line;
			CHECK(f.peer.expect("$MyNick ", line));
			CHECK(line == "$MyNick " + f.nick);
			printf("  ok the challenge names our own nick: %s\n", line.c_str());
		}
		else failures++;
	}

	{
		Fixture f;
		if (f.ready)
		{
			f.say("$MyNick peernick");
			f.say("$Lock " PEER_LOCK " Pk=PEER");

			std::string line;
			CHECK(f.peer.expect("$Lock ", line));
			CHECK(line.find(" Pk=") != std::string::npos);
			printf("  ok and carries a lock of our own\n");
		}
		else failures++;
	}

	/* A lock too short to derive a key from is refused, not answered. */
	{
		Fixture f;
		if (f.ready)
		{
			f.say("$MyNick peernick");
			f.say("$Lock ab Pk=PEER");
			pump([] { return false; }, 200);

			f.peer.drain();
			CHECK(f.peer.received.find("$Key") == std::string::npos);
			printf("  ok a lock too short to answer is refused\n");
		}
		else failures++;
	}

	/* Answering the challenge gets a direction and a key back. */
	{
		Fixture f;
		if (f.ready && f.handshake())
		{
			f.say("$Supports ADCGet TTHF");
			f.say("$Direction Download 12345");
			f.say("$Key somekey");

			std::string line;
			CHECK(f.peer.expect("$Direction ", line));
			printf("  ok the direction is negotiated: %s\n", line.c_str());
		}
		else failures++;
	}

	/* Nothing we hold, so the request is refused rather than answered. */
	{
		Fixture f;
		if (f.ready && f.handshake())
		{
			f.say("$Supports ADCGet TTHF");
			f.say("$Direction Download 12345");
			f.say("$Key somekey");
			f.say("$ADCGET file TTH/AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A 0 -1");

			std::string line;
			CHECK(f.peer.expect("$Error", line));
			printf("  ok a file we do not have is refused: %s\n", line.c_str());
		}
		else failures++;
	}

	/*
	 * What a peer can throw at us, before and after the handshake. The ADC
	 * side of this turned up a crash in the command parser, which runs before
	 * anything has been authorised.
	 */
	{
		Fixture f;
		if (f.ready)
		{
			/* Commands with nothing after the keyword. */
			f.say("$MyNick");
			f.say("$Lock");
			f.say("$Key");
			f.say("$Direction");
			f.say("$ADCGET");
			f.say("$ADCSND");
			f.say("$Supports");
			f.say("$Get");
			f.say("$Send");
			f.say("$GetZBlock");
			f.say("$GetListLen");
			f.say("$FileLength");
			f.say("$MaxedOut");
			f.say("$Error");
			f.say("$Failed");
			f.say("$Sending");

			/* A keyword and a single space. */
			f.say("$ADCGET ");
			f.say("$Direction ");
			f.say("$Lock ");

			/* Enough length, nothing sensible in it. */
			f.say("$ADCGET filenotanumberhere");
			f.say("$ADCGET file x y z");
			f.say("$ADCGET zzzz TTH/x 0 -1");
			f.say("$ADCSND file x");
			f.say("$Direction Sideways 12345");
			f.say("$Direction Upload notanumber");
			f.say("$Lock ab Pk=");

			/* Not NMDC at all. */
			f.say("");
			f.say("plain text");
			f.say("$NoSuchCommand with arguments");

			pump([] { return false; }, 300);
			CHECK(f.session != 0);
			printf("  ok malformed and unknown commands are survived\n");
		}
		else failures++;
	}

	/* The same, after the handshake, where more state is in play. */
	{
		Fixture f;
		if (f.ready && f.handshake())
		{
			f.say("$Supports ADCGet TTHF");
			f.say("$Direction Download 12345");
			f.say("$Key somekey");

			f.say("$ADCGET");
			f.say("$ADCGET ");
			f.say("$ADCGET file");
			f.say("$ADCGET file TTH/x");
			f.say("$ADCGET file TTH/x 0");
			f.say("$ADCGET list / 0 -1 ZL1");
			f.say("$ADCSND");
			f.say("$ADCSND file TTH/x 0 -1");
			f.say("$MyNick another");
			f.say("$Lock " PEER_LOCK " Pk=AGAIN");

			pump([] { return false; }, 300);
			CHECK(f.session != 0);
			printf("  ok malformed commands after the handshake are survived\n");
		}
		else failures++;
	}

	/* The peer hanging up is noticed rather than spun on. */
	{
		Fixture f;
		if (f.ready && f.handshake())
		{
			f.peer.local.reset();
			f.peer.mine.reset();
			pump([] { return false; }, 300);
			printf("  ok the peer hanging up is survived\n");
		}
		else failures++;
	}

	/* Connections come and go without leaving anything in the monitor. */
	{
		Samurai::IO::Net::SocketMonitor* monitor =
			Samurai::IO::Net::SocketMonitor::getInstance();
		const size_t baseline = monitor->size();

		for (int n = 0; n < 2; n++)
		{
			Fixture f;
			CHECK(f.ready);
			if (f.ready) f.handshake();
		}

		pump([&] { return monitor->size() <= baseline; }, 2000);
		CHECK(monitor->size() == baseline);
		printf("  ok repeated connections leave nothing behind\n");
	}
}

int main()
{
	setbuf(stdout, 0);

	const std::string cfg = "/tmp/quickdc_dcpeer_config.conf";
	const std::string ses = "/tmp/quickdc_dcpeer_session.conf";
	const std::string queue = "/tmp/quickdc_dcpeer_queue.xml";

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();
	Samurai::IO::File(queue).remove();

	{
		QuickDC::Core core(cfg, ses, queue);
		run();
	}

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();
	Samurai::IO::File(queue).remove();
	Samurai::IO::File(queue + ".new").remove();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
