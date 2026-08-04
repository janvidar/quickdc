/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The ADC hub server's resource ceilings, and the handful of one packet faults
 * that reach it before anything is authenticated.
 *
 * The hub here is the real one, listening on a loopback port the kernel picks,
 * driven by the real event loop - which is the only way to reach most of this,
 * since the hub does not authenticate and so has no state a test could set up
 * behind the protocol's back. What the clients send is what an attacker sends.
 *
 * The one thing that is not real is the core: the full one creates ~/.quickdc,
 * scans the shares and opens listening sockets of its own, none of which the hub
 * needs, so this uses the constructor that builds only what it does.
 *
 * That core is a singleton reading its configuration from disk, so it is a
 * per-file fixture. Everything else a case needs - the hub, the listening socket
 * and the clients - it builds itself and lets go of again, so no case depends on
 * what the one before it left behind.
 */

#include "quickdc.h"

#include "api/core.h"
#include "config/preferences.h"
#include "network/commandproxy.h"
#include "network/adc/adccommon.h"
#include "network/adc/cid.h"
#include "network/adc/parser.h"
#include "network/adc/server/adchub.h"
#include "network/adc/server/adchubuser.h"

#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/file.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/messagehandler.h>
#include <samurai/timer.h>
#include <samurai/util/base32.h>

#include <chrono>
#include <memory>
#include <span>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

static const int TIMEOUT_MS = 5000;

/* ------------------------------------------------------------------------- */

/*
 * Runs the event loop until the condition holds, or gives up.
 *
 * The timers and the message queue are turned by hand, because the core this
 * builds is the one that does not own them - and the hub needs both: the sweep
 * for connections that have gone quiet is a timer, and every disconnect it
 * decides on is posted rather than done on the spot.
 */
template<typename Fn>
static bool pump(Fn done, int timeout_ms = TIMEOUT_MS)
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);

	for (;;)
	{
		Samurai::TimerManager::getInstance()->process();
		Samurai::MessageHandler::getInstance()->process();

		if (done()) return true;
		if (std::chrono::steady_clock::now() > deadline) return done();

		monitor->wait(10);
	}
}

/*
 * A CID and the PID it is the hash of, which is what the hub checks. Each client
 * needs its own: the hub refuses a second connection presenting a CID that is
 * already logged in.
 */
struct Identity
{
	std::string cid;
	std::string pid;
};

static Identity makeIdentity(uint8_t seed)
{
	uint8_t raw[Samurai::Crypto::Digest::TIGER_HASH_SIZE];
	memset(raw, seed, sizeof(raw));

	char pid[64] = {0,};
	char cid[64] = {0,};

	Samurai::Util::base32_encode(
		std::span<const unsigned char>(raw, sizeof(raw)),
		std::span<char>(pid, sizeof(pid)));

	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(raw, sizeof(raw));
	tiger.digest().getFormattedString(
		Samurai::Crypto::Digest::HashValue::Format::Base32, cid, sizeof(cid));

	Identity id;
	id.cid = cid;
	id.pid = pid;
	return id;
}

/* The hub, and the listening socket that hands connections to it. */
class Server : public Samurai::IO::Net::ServerSocketEventHandler
{
	public:
		ADC::Hub hub;
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		size_t accepted = 0;

		Server()
		{
			hub.enabled = true;
		}

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			return server && server->getFD() != -1 && server->listen();
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }

		/* Replaces the hub's own sweep with one that fires often enough to watch. */
		void sweepEvery(std::chrono::milliseconds interval)
		{
			delete hub.timer;
			hub.timer = new Samurai::Timer(&hub, interval, false);
		}

		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }

		/* Exactly what the protocol probe does once it has recognised HSUP. */
		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			accepted++;
			hub.onConnected(socket);
			socket->setEventHandler(&hub);
		}
};

/* One connection to the hub, and whatever the hub has said on it. */
class Client : public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::Socket> socket;
		std::string received;
		std::string sid;

		/** Every line the hub has said, login exchange included. */
		std::vector<std::string> log;

		bool connected = false;
		bool disconnected = false;

		~Client() override
		{
			if (socket) socket->setEventHandler(0);
		}

		bool connect(uint16_t port)
		{
			socket = Samurai::IO::Net::Socket::create(this, std::string("127.0.0.1"), port);
			if (!socket) return false;

			socket->connect();
			return pump([&] { return connected || disconnected; });
		}

		void send(const std::string& line)
		{
			if (!socket) return;
			const std::string out = line + "\n";
			socket->write(out.data(), out.size());
		}

		/** Writes raw bytes, terminator and all left to the caller. */
		void sendRaw(const std::string& data)
		{
			if (!socket) return;
			socket->write(data.data(), data.size());
		}

		void drain()
		{
			if (!socket) return;
			for (;;)
			{
				char buf[4096];
				size_t got = 0;
				std::error_code ec;
				if (socket->read(buf, sizeof(buf), got, ec) != Samurai::IO::ReadResult::Ok) break;
				if (!got) break;
				received.append(buf, got);
			}
		}

		/** Waits for a whole line and takes it out of the stream. */
		bool nextLine(std::string& out, int timeout_ms = TIMEOUT_MS)
		{
			const bool got = pump([&] {
				drain();
				return received.find('\n') != std::string::npos;
			}, timeout_ms);

			if (!got) return false;

			const size_t at = received.find('\n');
			out = received.substr(0, at);
			received.erase(0, at + 1);
			log.push_back(out);
			return true;
		}

		/** True if any line the hub has said contains all of the given pieces. */
		bool sawLine(const std::vector<std::string>& pieces) const
		{
			for (size_t n = 0; n < log.size(); n++)
			{
				bool all = true;
				for (size_t p = 0; p < pieces.size() && all; p++)
					if (log[n].find(pieces[p]) == std::string::npos) all = false;

				if (all) return true;
			}
			return false;
		}

		/** Waits for a line whose first four bytes are the given fourcc. */
		bool waitFor(const char* fourcc, std::string& out, int timeout_ms = TIMEOUT_MS)
		{
			for (;;)
			{
				if (!nextLine(out, timeout_ms)) return false;
				if (out.compare(0, 4, fourcc) == 0) return true;
			}
		}

		/**
		 * The whole login exchange: support, the SID the hub hands out, and an
		 * INF - which the hub echoes back to everyone, this client included, and
		 * that echo is what says it is on.
		 */
		bool login(const Identity& id, const char* nick)
		{
			send("HSUP ADBASE ADTIGR");

			std::string line;
			if (!waitFor("ISID", line)) return false;
			if (line.size() < 9) return false;
			sid = line.substr(5, 4);

			send("BINF " + sid + " ID" + id.cid + " PD" + id.pid + " NI" + nick);

			for (;;)
			{
				if (!nextLine(line)) return false;
				if (line.compare(0, 4, "BINF") == 0 && line.find(sid) != std::string::npos)
					return true;
				if (line.compare(0, 4, "ISTA") == 0 && line.find(" 2") != std::string::npos)
					return false;
			}
		}

		void EventHostLookup(const Samurai::IO::Net::Socket*) override { }
		void EventHostFound(const Samurai::IO::Net::Socket*) override { }
		void EventConnecting(const Samurai::IO::Net::Socket*) override { }
		void EventConnected(const Samurai::IO::Net::Socket*) override { connected = true; }
		void EventTimeout(const Samurai::IO::Net::Socket*) override { }
		void EventDisconnected(const Samurai::IO::Net::Socket*) override { disconnected = true; }
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { }
		void EventCanWrite(const Samurai::IO::Net::Socket*) override { }
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override { disconnected = true; }
};

/* ------------------------------------------------------------------------- */

/*
 * The fixture. Scratch paths of its own, so two of these running at once are not
 * reading and rewriting the same configuration.
 */
static std::string cfg;
static std::string ses;
static QuickDC::Core* core = 0;

static std::string scratch(const char* leaf)
{
	return "/tmp/quickdc_adchubtest_" + std::to_string((int) getpid()) + "_" + leaf;
}

EXO_SETUP({
	cfg = scratch("config.conf");
	ses = scratch("session.conf");

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	core = new QuickDC::Core(cfg, ses);
	return 1;
});

EXO_TEARDOWN({
	/* Before the files: Preferences writes itself out when it is destroyed, and
	   the core holds both of them open until then. */
	delete core;
	core = 0;

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();
	return 1;
});

/* ------------------------------------------------------------------------- */

/* How many arguments one command may carry. */
EXO_TEST(adchub_a_command_carries_at_most_max_command_arguments,
{
	/* As many as are allowed, counting the source ID the 'B' prefix consumes. */
	std::string ok = "BINF AAAA";
	for (size_t n = 1; n < ADC::MAX_COMMAND_ARGUMENTS; n++) ok += " AA1";

	ADC::Command accepted;
	EXO_ASSERT(ADC::Parser::parse(ok.c_str(), ok.size(), accepted));
	EXO_ASSERT(accepted.countArguments() == ADC::MAX_COMMAND_ARGUMENTS - 1);

	const std::string one_too_many = ok + " AA1";
	ADC::Command refused;
	EXO_ASSERT(!ADC::Parser::parse(one_too_many.c_str(), one_too_many.size(), refused));

	/*
	 * And the case it exists for: a line of the length one packet can hold,
	 * entirely made of INF arguments. Each of these used to have the whole
	 * accumulated INF deep copied for it, and the copy leaked.
	 */
	std::string flood = "BINF AAAA";
	while (flood.size() < 128 * 1024) flood += " AA1";

	ADC::Command bomb;
	EXO_ASSERT(!ADC::Parser::parse(flood.c_str(), flood.size(), bomb));
	return 1;
});

/* What the send queue may hold before it refuses. */
EXO_TEST(adchub_the_send_queue_refuses_past_its_ceiling,
{
	QuickDC::CommandFIFO fifo;

	/* No ceiling unless one is asked for, which is what the client sessions
	   sharing this queue expect. */
	EXO_ASSERT(fifo.getMaxQueueSize() == 0);

	fifo.setMaxQueueSize(1024);

	size_t queued = 0;
	size_t refused = 0;

	for (int n = 0; n < 1000; n++)
	{
		ADC::Command* cmd = new ADC::Command(FOURCC('I','M','S','G'));
		cmd->addArgument("a message nobody is reading");

		/* Held over the call, so a refusal has nothing left holding it. */
		cmd->incRef();
		if (fifo.push(cmd)) queued++;
		else refused++;
		cmd->decRef();

		if (cmd->canDelete()) delete cmd;
	}

	EXO_ASSERT(queued > 0);
	EXO_ASSERT(refused > 0);
	EXO_ASSERT(queued + refused == 1000);
	EXO_ASSERT(fifo.getQueueSize() <= 1024);
	return 1;
});

/* ------------------------------------------------------------------------- */

/* A line that never ends. */
EXO_TEST(adchub_a_line_that_is_never_terminated_closes_the_connection,
{
	Server s;
	EXO_ASSERT(s.listen());

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(pump([&] { return s.hub.users.size() == 1; }));

	/* Enough to be recognised, and then nothing that ends a line. */
	c.send("HSUP ADBASE ADTIGR");

	std::string line;
	EXO_ASSERT(c.waitFor("ISID", line));

	c.sendRaw(std::string(32 * 1024, 'A'));

	EXO_ASSERT(pump([&] { return c.disconnected; }));
	EXO_ASSERT(pump([&] { return s.hub.users.empty(); }));
	return 1;
});

/* The features a user may advertise, and what happens when it advertises none. */
EXO_TEST(adchub_a_user_advertises_at_most_max_user_features,
{
	Server s;
	EXO_ASSERT(s.listen());

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(pump([&] { return s.hub.users.size() == 1; }));
	EXO_ASSERT(!s.hub.users.empty());

	ADC::HubUser* user = s.hub.users[0];

	/* A null feature is not a feature, and asking must not read through it. */
	EXO_ASSERT(!user->hasFeature(0));
	EXO_ASSERT(!user->hasFeature("TOOLONG"));
	EXO_ASSERT(!user->hasFeature("AB"));

	user->clearFeatures();
	user->addFeatures("TIGR,ADCS,BZIP");
	EXO_ASSERT(user->countFeatures() == 3);
	EXO_ASSERT(user->hasFeature("TIGR"));
	EXO_ASSERT(user->hasFeature("ADCS"));
	EXO_ASSERT(user->hasFeature("BZIP"));
	EXO_ASSERT(!user->hasFeature("SEGA"));

	/* And the field as an attacker writes it. */
	user->clearFeatures();
	std::string many;
	while (many.size() < 128 * 1024) many += "AA1,";
	user->addFeatures(many.c_str());
	EXO_ASSERT(user->countFeatures() <= ADC::MAX_USER_FEATURES);
	return 1;
});

/* Every SID the hub hands out has to be one it can find again. */
EXO_TEST(adchub_every_session_id_handed_out_resolves_back_to_its_user,
{
	Server s;
	EXO_ASSERT(s.listen());

	/*
	 * Started just below the 20 bits a SID has, so the next few allocations run
	 * over the edge. A SID is handed to the client as four base32 characters and
	 * every private message is routed by turning those back into a number, so a
	 * SID that does not survive that round trip is a user nobody can reach.
	 */
	s.hub.availSid = ADC::SID_MASK - 2;
	s.hub.maxPerAddress = 16;

	std::vector<Client*> clients;
	for (int n = 0; n < 6; n++)
	{
		Client* c = new Client();
		clients.push_back(c);
		EXO_ASSERT(c->connect(s.port()));
	}

	EXO_ASSERT(pump([&] { return s.hub.users.size() == clients.size(); }));
	EXO_ASSERT(s.hub.sid_map.size() == s.hub.users.size());

	for (size_t n = 0; n < s.hub.users.size(); n++)
	{
		const sid_t sid = s.hub.users[n]->getSID();

		if (sid == 0)
			EXO_FAIL("a user was handed session ID zero");

		if (sid != (sid & ADC::SID_MASK))
		{
			const std::string why = "session ID " + std::to_string(sid)
				+ " does not fit the twenty bits a SID has";
			EXO_FAIL(why.c_str());
		}

		/* The route a D or an E message takes to find its target. */
		if (s.hub.lookupUser(ADC::SID::fromString(ADC::SID::toString(sid)))
			!= s.hub.users[n])
		{
			const std::string why = "session ID " + std::to_string(sid)
				+ " does not survive the round trip through its four characters";
			EXO_FAIL(why.c_str());
		}
	}

	for (size_t n = 0; n < clients.size(); n++) delete clients[n];
	return 1;
});

/* One packet faults that a logged in user can send. */
EXO_TEST(adchub_a_message_with_no_message_does_not_take_the_hub_down,
{
	Server s;
	EXO_ASSERT(s.listen());

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.connected);
	EXO_ASSERT(c.login(makeIdentity(1), "tester"));

	/*
	 * 'BMSG <sid>' parses cleanly and carries no message at all: the source ID
	 * consumes the only argument. The hub used to hand what was left - nothing -
	 * to strlen().
	 */
	c.send("BMSG " + c.sid);

	/* And a few more shapes with nothing where something is expected. */
	c.send("BMSG");
	c.send("BSCH " + c.sid);
	c.send("DMSG " + c.sid);
	c.send("HMSG");
	c.send("BINF " + c.sid);
	c.send("");
	c.send("not a command at all");

	/* Still there, and still talking. */
	c.send("BMSG " + c.sid + " still\\salive");

	std::string line;
	EXO_ASSERT(c.waitFor("BMSG", line));
	EXO_ASSERT(line.find("still\\salive") != std::string::npos);
	EXO_ASSERT(s.hub.users.size() == 1);
	return 1;
});

/* An INF that is updated over and over. */
EXO_TEST(adchub_an_inf_is_accumulated_across_updates_and_handed_on_whole,
{
	Server s;
	EXO_ASSERT(s.listen());

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(8), "alice"));

	/*
	 * Each of these is folded into the one INF the hub keeps for alice, which is
	 * what it hands to whoever arrives later. One copy per update, with the
	 * reference this user holds moving over to it - a copy per argument, taking a
	 * fresh reference each time, is the same INF left behind on every round.
	 */
	std::string line;
	for (int n = 0; n < 50; n++)
	{
		alice.send("BINF " + alice.sid + " DEround" + std::to_string(n)
			+ " SS" + std::to_string(n));

		if (!alice.waitFor("BINF", line))
		{
			const std::string why = "no INF echoed back for round "
				+ std::to_string(n);
			EXO_FAIL(why.c_str());
		}
	}

	Client bob;
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(9), "bob"));

	/* Everything alice ever said about herself, in one command. */
	EXO_ASSERT(bob.sawLine({"BINF", alice.sid, "NIalice", "DEround49", "SS49"}));
	return 1;
});

/* A user that claims a session ID that is not its own. */
EXO_TEST(adchub_a_mismatched_source_sid_retires_the_connection_and_says_whose_it_was,
{
	Server s;
	EXO_ASSERT(s.listen());

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(2), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(3), "bob"));

	/* A source SID naming nobody, which is what leaves the hub with no user to
	   name in what it tells everyone else. */
	bob.send("DMSG ZZZZ " + alice.sid + " a\\sword\\sfrom\\snobody");

	std::string line;
	EXO_ASSERT(bob.waitFor("ISTA", line));
	EXO_ASSERT(line.find("240") != std::string::npos);

	/*
	 * And the others are told bob is gone, by bob's own SID. Without it every
	 * remaining user keeps an entry for a user that is no longer there.
	 */
	EXO_ASSERT(alice.waitFor("IQUI", line));
	EXO_ASSERT(line.find(bob.sid) != std::string::npos);

	EXO_ASSERT(pump([&] { return s.hub.users.size() == 1; }));
	return 1;
});

/* How many connections, and from where. */
EXO_TEST(adchub_connections_past_the_per_address_limit_do_not_become_users,
{
	Server s;
	EXO_ASSERT(s.listen());

	s.hub.maxPerAddress = 3;

	std::vector<Client*> clients;
	for (int n = 0; n < 6; n++)
	{
		Client* c = new Client();
		clients.push_back(c);
		c->connect(s.port());
	}

	/* Every one of them was accepted by the kernel; only three became users. */
	EXO_ASSERT(pump([&] { return s.accepted == clients.size(); }));
	EXO_ASSERT(pump([&] { return s.hub.users.size() == 3; }, 1000));
	EXO_ASSERT(s.hub.users.size() == 3);

	for (size_t n = 0; n < clients.size(); n++) delete clients[n];
	return 1;
});

EXO_TEST(adchub_connections_past_the_handshake_limit_do_not_become_users,
{
	Server s;
	EXO_ASSERT(s.listen());

	s.hub.maxPerAddress = 64;
	s.hub.maxHandshake = 2;

	std::vector<Client*> clients;
	for (int n = 0; n < 5; n++)
	{
		Client* c = new Client();
		clients.push_back(c);
		c->connect(s.port());
	}

	EXO_ASSERT(pump([&] { return s.accepted == clients.size(); }));
	EXO_ASSERT(pump([&] { return s.hub.users.size() == 2; }, 1000));
	EXO_ASSERT(s.hub.countHandshaking() == 2);

	for (size_t n = 0; n < clients.size(); n++) delete clients[n];
	return 1;
});

/* A connection that says the four bytes the probe wants, and then nothing. */
EXO_TEST(adchub_a_connection_that_never_logs_in_is_swept_away,
{
	Server s;
	EXO_ASSERT(s.listen());

	s.hub.handshakeTimeout = 1;
	s.sweepEvery(std::chrono::milliseconds(50));

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(pump([&] { return s.hub.users.size() == 1; }));

	/* Nothing else is ever sent. */
	EXO_ASSERT(pump([&] { return c.disconnected; }, 6000));
	EXO_ASSERT(pump([&] { return s.hub.users.empty(); }));
	return 1;
});

/* And one that logs in and then goes quiet. */
EXO_TEST(adchub_a_logged_in_user_that_goes_quiet_is_swept_away,
{
	Server s;
	EXO_ASSERT(s.listen());

	s.hub.handshakeTimeout = 0;
	s.hub.idleTimeout = 1;
	s.sweepEvery(std::chrono::milliseconds(50));

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(4), "quiet"));

	EXO_ASSERT(pump([&] { return c.disconnected; }, 6000));
	EXO_ASSERT(pump([&] { return s.hub.users.empty(); }));
	return 1;
});

/* A hub that is full, and who is let in anyway. */
EXO_TEST(adchub_an_operator_is_let_into_a_full_hub,
{
	Server s;
	EXO_ASSERT(s.listen());

	s.hub.maxUsers = 1;

	/*
	 * Both of these connect from the loopback address, which the hub gives
	 * administrator credentials - so both are exempt from the limit. Written as
	 * an 'or' the exemption held for everyone, which is to say for nobody.
	 */
	Client first;
	Client second;
	EXO_ASSERT(first.connect(s.port()));
	EXO_ASSERT(first.login(makeIdentity(5), "first"));
	EXO_ASSERT(second.connect(s.port()));
	EXO_ASSERT(second.login(makeIdentity(6), "second"));

	EXO_ASSERT(s.hub.countLoggedIn() == 2);
	return 1;
});

/* What the chat history keeps. */
EXO_TEST(adchub_the_chat_history_keeps_what_it_is_configured_to,
{
	Server s;
	EXO_ASSERT(s.listen());

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(7), "talker"));
	EXO_ASSERT(!s.hub.users.empty());

	ADC::HubUser* user = s.hub.users[0];

	/* A message with nothing in it is not one. */
	s.hub.onPublicChat(user, 0);
	EXO_ASSERT(s.hub.history.empty());

	s.hub.maxHistory = 3;
	for (int n = 0; n < 20; n++) s.hub.onPublicChat(user, "something");
	EXO_ASSERT(s.hub.history.size() == 3);

	/*
	 * Configured to keep none keeps none. Read the other way - as a ring whose
	 * rotation test compares against 0 minus one - the log grows for as long as
	 * the hub runs.
	 */
	while (s.hub.history.size())
	{
		char* remove = s.hub.history.front();
		s.hub.history.pop_front();
		free(remove);
	}

	s.hub.maxHistory = 0;
	for (int n = 0; n < 20; n++) s.hub.onPublicChat(user, "something");
	EXO_ASSERT(s.hub.history.empty());
	return 1;
});

/* Every configured limit is one the hub can work with. */
EXO_TEST(adchub_a_configured_limit_that_makes_no_sense_is_clamped,
{
	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Local hub");
	config->setNumber("Max users", 0);
	config->setNumber("Max history", -1);
	config->setNumber("Max send queue bytes", -4096);
	config->setNumber("Max connections per address", 0);
	config->setNumber("Max connections in handshake", -7);
	config->setNumber("Handshake timeout", -1);
	config->setNumber("Idle timeout", -1);

	{
		ADC::Hub hub;
		EXO_ASSERT(hub.maxUsers >= 1);
		EXO_ASSERT(hub.maxHistory == 0);
		EXO_ASSERT(hub.maxSendQ >= 4096);
		EXO_ASSERT(hub.hardSendQ > hub.maxSendQ);
		EXO_ASSERT(hub.maxPerAddress >= 1);
		EXO_ASSERT(hub.maxHandshake >= 1);
		EXO_ASSERT(hub.handshakeTimeout == 0);
		EXO_ASSERT(hub.idleTimeout == 0);
	}

	config->setGroup("Local hub");
	config->remove("Max users");
	config->remove("Max history");
	config->remove("Max send queue bytes");
	config->remove("Max connections per address");
	config->remove("Max connections in handshake");
	config->remove("Handshake timeout");
	config->remove("Idle timeout");
	return 1;
});

/* Users come and go without leaving anything in the socket monitor. */
EXO_TEST(adchub_hubs_come_and_go_leaving_nothing_behind,
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();
	const size_t baseline = monitor->size();

	for (int n = 0; n < 3; n++)
	{
		Server s;
		EXO_ASSERT(s.listen());

		Client c;
		EXO_ASSERT(c.connect(s.port()));
		EXO_ASSERT(c.login(makeIdentity((uint8_t) (10 + n)), "comer"));
	}

	pump([&] { return monitor->size() <= baseline; }, 2000);
	EXO_ASSERT(monitor->size() == baseline);
	return 1;
});
