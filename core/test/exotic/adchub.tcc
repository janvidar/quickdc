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
#include "network/adc/server/hub.h"
#include "network/adc/server/service.h"
#include "network/adc/server/user.h"
#include "network/adc/server/claimcheck.h"

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
#include <errno.h>
#include <map>
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

/*
 * An argument as it appears on the wire: a space is escaped on the way out, so a
 * phrase looked for in a raw line has to be spelled the same way.
 */
static std::string escapedArgument(const std::string& text)
{
	std::string out;
	for (size_t n = 0; n < text.size(); n++)
	{
		if (text[n] == ' ') out += "\\s";
		else out += text[n];
	}
	return out;
}

/*
 * The hub, with the socket error handler reachable.
 *
 * The socket monitor is the only other caller of it, and no case can make the
 * kernel report a chosen errno on demand - so it is called with exactly what
 * samurai would have handed it.
 */
class TestHub : public ADC::LocalHub::Hub
{
	public:
		using ADC::LocalHub::Hub::EventError;

		/* What a hub does at startup, so a case can watch it read a setting. */
		using ADC::LocalHub::Hub::readConfiguration;
};

/* The hub, and the listening socket that hands connections to it. */
class Server : public Samurai::IO::Net::ServerSocketEventHandler
{
	public:
		TestHub hub;
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		size_t accepted = 0;

		Server()
		{
			hub.enabled = true;

			/*
			 * The record of which endpoints have been checked belongs to the
			 * process, not to a hub, so one case's checks would otherwise be
			 * the reason the next case's are declined.
			 */
			ADC::LocalHub::ClaimCheck::reset();
		}

		/*
		 * Whether the kernel gave us a bound socket, which is about the machine
		 * and not about the hub - so a case whose listen() fails skips rather
		 * than reporting the code under test as broken.
		 */
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
		bool login(const Identity& id, const char* nick, const char* extra = 0)
		{
			send("HSUP ADBASE ADTIGR");

			std::string line;
			if (!waitFor("ISID", line)) return false;
			if (line.size() < 9) return false;
			sid = line.substr(5, 4);

			std::string inf = "BINF " + sid + " ID" + id.cid + " PD" + id.pid
				+ " NI" + nick;
			if (extra) { inf += " "; inf += extra; }
			send(inf);

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
		/* Read as soon as it is offered rather than only when a case asks for it:
		   the monitor is level triggered, so a socket left unread is reported on
		   every pass and the loop spins until something takes the bytes off it. */
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { drain(); }
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

	/* Every case reads the hub's limits through this, so a core without one is
	   not a fixture the cases can run against. */
	return (QuickDC::Core::getInstance() == core && core->config) ? 1 : 0;
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
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

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
/*
 * What the hub claims of itself. PING and NATT say it answers a pinger and relays a
 * traversal, both of which are services a hub runs and a client has no business
 * offering - so neither comes out of the shared support flags, and this is where they
 * are said instead.
 */
EXO_TEST(adchub_the_hub_offers_to_answer_pingers,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));

	c.send("HSUP ADBASE ADTIGR");

	std::string sup;
	EXO_ASSERT(c.waitFor("ISUP", sup));

	EXO_ASSERT(sup.find("AD" ADC_EXT_PING) != std::string::npos);
	EXO_ASSERT(sup.find("AD" ADC_EXT_NATT) != std::string::npos);

	/* And the rich text a hub answers with, which is not ours to claim alone. */
	EXO_ASSERT(sup.find("AD" ADC_EXT_RICHTEXT) != std::string::npos);
	return 1;
});

EXO_TEST(adchub_a_user_advertises_at_most_max_user_features,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(pump([&] { return s.hub.users.size() == 1; }));
	EXO_ASSERT(!s.hub.users.empty());

	ADC::LocalHub::User* user = s.hub.users[0];

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
	EXO_ASSERT(user->countFeatures() <= ADC::LocalHub::MAX_USER_FEATURES);
	return 1;
});

/* Every SID the hub hands out has to be one it can find again. */
EXO_TEST(adchub_every_session_id_handed_out_resolves_back_to_its_user,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	/*
	 * Started just below the 20 bits a SID has, so the next few allocations run
	 * over the edge. A SID is handed to the client as four base32 characters and
	 * every private message is routed by turning those back into a number, so a
	 * SID that does not survive that round trip is a user nobody can reach.
	 */
	s.hub.availSid = ADC::LocalHub::SID_MASK - 2;
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

		if (sid != (sid & ADC::LocalHub::SID_MASK))
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
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

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
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

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
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

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

/*
 * A connection that fails, and what the rest of the hub is told about it.
 *
 * samurai reports a socket error with strerror() of the local errno, and the hub
 * put that text straight into the MS field of the IQUI it broadcasts - so one
 * peer's errno, and with it the host's operating system and its error
 * conditions, was published to every other user on the hub. What they are told
 * now is a phrase of the hub's own; the errno is logged and goes nowhere else.
 */
EXO_TEST(adchub_a_socket_error_is_not_repeated_to_the_other_users,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(20), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(21), "bob"));

	/* The hub's end of bob's connection, which is what the monitor would name. */
	const sid_t sid = ADC::SID::fromString(bob.sid.c_str());
	const Samurai::IO::Net::Socket* peer = 0;
	std::map<const Samurai::IO::Net::Socket*, ADC::LocalHub::User*>::iterator it;
	for (it = s.hub.socket_map.begin(); it != s.hub.socket_map.end(); it++)
		if ((*it).second->getSID() == sid) peer = (*it).first;
	EXO_ASSERT(peer != 0);

	/* The C library's text for a reset, which is what samurai passes on. */
	const std::string errno_text = strerror(ECONNRESET);
	EXO_ASSERT(!errno_text.empty());

	s.hub.EventError(peer, Samurai::IO::Net::SocketError::SocketRead,
		errno_text.c_str());

	/* Alice is told bob is gone, by bob's SID - that much she needs. */
	std::string line;
	EXO_ASSERT(alice.waitFor("IQUI", line));
	EXO_ASSERT(line.find(bob.sid) != std::string::npos);

	/* Nothing the hub ever said to alice carries the errno text, raw or escaped. */
	if (alice.sawLine({errno_text}) || alice.sawLine({escapedArgument(errno_text)}))
	{
		const std::string why = "the peer's errno text reached another user: '"
			+ errno_text + "'";
		EXO_FAIL(why.c_str());
	}

	/* And the reason given is the hub's own phrase. */
	EXO_ASSERT(line.find("MS" + escapedArgument(ADC_STR_CONNECTION_LOST))
		!= std::string::npos);

	EXO_ASSERT(pump([&] { return s.hub.users.size() == 1; }));
	return 1;
});

/* How many connections, and from where. */
EXO_TEST(adchub_connections_past_the_per_address_limit_do_not_become_users,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

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

/*
 * A connection with no peer, which is what a socket that never connected is.
 *
 * getAddress() answers null for one, and refusing a connection is exactly when
 * the hub says where it came from - so both of those lines dereferenced null on
 * a peer that had gone by the time it was turned away. Only a tracing build
 * compiles the hub log at all, which is why this survived: it crashed there and
 * nowhere else.
 */
EXO_TEST(adchub_a_connection_with_no_peer_is_refused_rather_than_crashed_on,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	/* Made, not connected: there is no peer, so there is no address. */
	std::shared_ptr<Samurai::IO::Net::Socket> lonely =
		Samurai::IO::Net::Socket::create(
			static_cast<Samurai::IO::Net::SocketEventHandler*>(0),
			std::string("127.0.0.1"), s.port());

	if (!lonely) EXO_SKIP("could not create a socket");
	EXO_ASSERT(lonely->getAddress() == 0);

	/* Every connection is one too many, which is the branch that reports the
	   count and where the connection came from. */
	s.hub.maxUsers = 0;
	s.hub.maxHandshake = 0;
	EXO_ASSERT(!s.hub.admitConnection(lonely.get()));

	/* And the one that reports the login exchange it could not join. */
	s.hub.maxUsers = 128;
	EXO_ASSERT(!s.hub.admitConnection(lonely.get()));

	/*
	 * And with room to spare it is still refused, because the per-address limit
	 * cannot be applied to an address that cannot be read - skipping the check
	 * would have made this the one connection the limit never bounded.
	 */
	s.hub.maxHandshake = 8;
	s.hub.maxPerAddress = 8;
	EXO_ASSERT(!s.hub.admitConnection(lonely.get()));

	/* A connection that is not a Socket at all is not admitted either. */
	EXO_ASSERT(!s.hub.admitConnection(0));
	return 1;
});

EXO_TEST(adchub_connections_past_the_handshake_limit_do_not_become_users,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

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
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

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
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

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

/*
 * A user on its way out, and what becomes of what it goes on sending.
 *
 * kick() leaves a user that still has something queued to EventCanWrite, so that
 * the message saying why it was kicked reaches it. Until that happens the user is
 * in State_WaitDisconnect - and read() used to answer a readiness report from one
 * of those by returning, which left the input where it was. The monitor is level
 * triggered, so it was reported again on the next pass and on every pass after
 * that: the hub burned a processor for as long as the user sat there.
 */
static ADC::LocalHub::User* userFor(ADC::LocalHub::Hub& hub, const std::string& sid,
                                    const Samurai::IO::Net::Socket*& peer)
{
	const sid_t want = ADC::SID::fromString(sid.c_str());

	std::map<const Samurai::IO::Net::Socket*, ADC::LocalHub::User*>::iterator it;
	for (it = hub.socket_map.begin(); it != hub.socket_map.end(); it++)
	{
		if ((*it).second->getSID() != want) continue;
		peer = (*it).first;
		return (*it).second;
	}
	return 0;
}

/*
 * Queued without being flushed, which is the state kick() defers: with nothing
 * outstanding it posts the disconnect and the user is gone before a case can look
 * at it. Nothing arms the write notifier either, so the queue stays put - which is
 * exactly the peer that never reads, without needing one.
 */
static void kickWithSomethingQueued(ADC::LocalHub::Hub& hub, ADC::LocalHub::User* user)
{
	ADC::Command* msg = new ADC::Command(FOURCC('I','M','S','G'));
	msg->addArgument("still\\squeued");
	hub.send(msg, user, true);
	hub.kick(user);
}

EXO_TEST(adchub_a_kicked_user_does_not_leave_its_input_on_the_socket,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(40), "alice"));

	const Samurai::IO::Net::Socket* peer = 0;
	ADC::LocalHub::User* user = userFor(s.hub, alice.sid, peer);
	EXO_ASSERT(user != 0 && peer != 0);

	kickWithSomethingQueued(s.hub, user);
	EXO_ASSERT(user->getSendQueueSize() != 0);
	EXO_ASSERT(user->getState() == ADC::LocalHub::State_WaitDisconnect);

	/* Nothing it says now will be acted on, which is the point of the state - but
	   it has to come off the socket all the same. */
	alice.send("BMSG " + alice.sid + " nobody\\sis\\slistening");
	pump([&] { return false; }, 300);

	/* Still the user this is about, and its input is gone from the hub's end. */
	EXO_ASSERT(user->getState() == ADC::LocalHub::State_WaitDisconnect);

	char peeked = 0;
	EXO_ASSERT(::recv(peer->getFD(), &peeked, 1, MSG_PEEK | MSG_DONTWAIT) < 0);
	return 1;
});

/*
 * And the wait has an end to it.
 *
 * What kick() defers to depends on the peer making room by reading, so a peer that
 * has stopped reading never produces it - and the sweep used to skip the state
 * outright, which left such a user in the hub's tables, holding a descriptor and a
 * session ID, for the life of the process.
 */
EXO_TEST(adchub_a_kicked_user_that_never_takes_its_message_goes_anyway,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	s.hub.handshakeTimeout = 0;
	s.hub.idleTimeout = 0;
	s.hub.lingerTimeout = 1;
	s.sweepEvery(std::chrono::milliseconds(50));

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(41), "alice"));

	const Samurai::IO::Net::Socket* peer = 0;
	ADC::LocalHub::User* user = userFor(s.hub, alice.sid, peer);
	EXO_ASSERT(user != 0);

	kickWithSomethingQueued(s.hub, user);
	EXO_ASSERT(user->getSendQueueSize() != 0);

	EXO_ASSERT(pump([&] { return s.hub.users.empty(); }, 6000));
	return 1;
});

/* A hub that is full, and who is let in anyway. */
/*
 * A full hub turns an ordinary user away.
 *
 * This could not be tested until recently: every client here connects from the
 * loopback address, and the hub gave a loopback client administrator
 * credentials unconditionally - which exempted it from the limit. So the limit
 * was never reached by anything, and this asserts it now that the grant is
 * behind QUICKDC_ALLOW_LOOPBACK_AUTH.
 */
EXO_TEST(adchub_a_full_hub_turns_an_ordinary_user_away,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	s.hub.maxUsers = 1;

	Client first;
	Client second;
	EXO_ASSERT(first.connect(s.port()));
	EXO_ASSERT(first.login(makeIdentity(5), "first"));
	EXO_ASSERT(second.connect(s.port()));
	second.login(makeIdentity(6), "second");

#ifdef QUICKDC_ALLOW_LOOPBACK_AUTH
	/*
	 * Both are administrators in this build, and an administrator is exempt -
	 * the exemption is an 'and', so written as an 'or' it held for everyone,
	 * which is to say for nobody.
	 */
	EXO_ASSERT(s.hub.countLoggedIn() == 2);
#else
	/* The second is refused, and the first is left alone. */
	EXO_ASSERT(s.hub.countLoggedIn() == 1);
#endif
	return 1;
});

/*
 * The service that tells a user where it is connecting from.
 *
 * It asked the socket for its address and formatted the answer without looking,
 * so a peer that had gone by the time the question was processed crashed the hub
 * - and this one is not a log line, so it crashed every build.
 */
EXO_TEST(adchub_the_address_service_answers_a_connection_with_no_peer,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(22), "asker"));

	c.send("BMSG " + c.sid + " +myip");

	std::string line;
	bool answered = false;
	for (int n = 0; n < 10 && !answered; n++)
	{
		if (!c.nextLine(line)) break;
		answered = line.find("Your\\sIP\\saddress") != std::string::npos;
	}

	EXO_ASSERT(answered);
	EXO_ASSERT(line.find("127.0.0.1") != std::string::npos);

	/* The same question asked over a connection that has no peer. */
	std::shared_ptr<Samurai::IO::Net::Socket> lonely =
		Samurai::IO::Net::Socket::create(
			static_cast<Samurai::IO::Net::SocketEventHandler*>(0),
			std::string("127.0.0.1"), s.port());

	if (!lonely) EXO_SKIP("could not create a socket");
	EXO_ASSERT(lonely->getAddress() == 0);

	ADC::Command ask(FOURCC('B','M','S','G'), c.sid.c_str(), 0);
	ask.setSocket(lonely.get());
	ask.addArgument("+myip");
	EXO_ASSERT(s.hub.services->invoke(&ask));

	answered = false;
	for (int n = 0; n < 10 && !answered; n++)
	{
		if (!c.nextLine(line)) break;
		answered = line.find("Your\\sIP\\saddress\\sis\\snot\\sknown") != std::string::npos;
	}
	EXO_ASSERT(answered);
	return 1;
});

/* What the chat history keeps. */
EXO_TEST(adchub_the_chat_history_keeps_what_it_is_configured_to,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(7), "talker"));
	EXO_ASSERT(!s.hub.users.empty());

	ADC::LocalHub::User* user = s.hub.users[0];

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
		ADC::LocalHub::Hub hub;
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
		if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

		Client c;
		EXO_ASSERT(c.connect(s.port()));
		EXO_ASSERT(c.login(makeIdentity((uint8_t) (10 + n)), "comer"));
	}

	pump([&] { return monitor->size() <= baseline; }, 2000);
	EXO_ASSERT(monitor->size() == baseline);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What the hub says about a claimed address                                 */
/* ------------------------------------------------------------------------- */

/*
 * A client claiming an address the hub does not see it at is told so, and told
 * where it actually is - which is how a client behind a carrier-grade NAT learns
 * an address that reaches it, since its own gateway can only report the address
 * of its own outside.
 *
 * The code matters as much as the message. 146 is the one that carries 'IP'; this
 * used to answer 145, which is the feature error and asks for 'FC' - a client
 * reading that has been told something unrecoverable rather than told its address,
 * and QuickDC's own client stops reconnecting on it.
 */
EXO_TEST(adchub_a_claimed_address_the_hub_does_not_see_is_answered_with_the_real_one,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(21), "alice"));

	/* Not where the hub sees her: she connected over the loopback. */
	alice.send("BINF " + alice.sid + " I4198.51.100.9");

	std::string line;
	EXO_ASSERT(alice.waitFor("ISTA", line));

	EXO_ASSERT(line.find("146") != std::string::npos);
	EXO_ASSERT(line.find("IP127.0.0.1") != std::string::npos);
	return 1;
});

/* And 0.0.0.0 is a question rather than a claim: the hub answers it by filling in
   what it sees, with no complaint at all. */
EXO_TEST(adchub_an_unspecified_address_is_replaced_rather_than_refused,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(22), "alice"));

	alice.send("BINF " + alice.sid + " I40.0.0.0");

	std::string line;
	EXO_ASSERT(alice.waitFor("BINF", line));

	/* Substituted, and nothing was refused. */
	EXO_ASSERT(line.find("I4127.0.0.1") != std::string::npos);
	EXO_ASSERT(line.find("I40.0.0.0") == std::string::npos);

	/* Substituted, and nothing was refused. */
	EXO_ASSERT(line.find("I4127.0.0.1") != std::string::npos);
	EXO_ASSERT(line.find("I40.0.0.0") == std::string::npos);
	return 1;
});

/*
 * An update that changed exactly one thing reaches the other users.
 *
 * The relay asked for more than one argument, so a single-field INF was
 * accumulated into what the hub holds and then told to nobody - which is most
 * updates a running client sends: a share size changing on its own, or a
 * description. It is also what makes the address question above answerable.
 */
EXO_TEST(adchub_an_update_of_a_single_field_still_reaches_everyone,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(23), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(24), "bob"));

	alice.send("BINF " + alice.sid + " SS4096");

	std::string line;
	EXO_ASSERT(bob.waitFor("BINF", line));
	EXO_ASSERT(line.find(alice.sid) != std::string::npos);
	EXO_ASSERT(line.find("SS4096") != std::string::npos);
	return 1;
});

/*
 * And one that changed nothing reaches nobody. Enforced by setINF() answering
 * with the number of fields it took rather than by the relay, which is why the
 * relay itself is unconditional - asserted here as behaviour so it stays true
 * whichever of the two is doing the work.
 */
EXO_TEST(adchub_an_update_of_nothing_is_not_relayed,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(25), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(26), "bob"));

	bob.drain();
	bob.received.clear();

	alice.send("BINF " + alice.sid);

	pump([] { return false; }, 300);
	bob.drain();
	EXO_ASSERT(bob.received.find("BINF") == std::string::npos);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* NAT traversal, relayed                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Two clients that neither of them can be connected to reach each other by dialling
 * at once, which they arrange over the hub because the hub is the only path between
 * them. The hub's part is to pass DNAT and DRNT along, exactly as it passes CTM and
 * RCM.
 *
 * Unlike an address, there is nothing here the hub could check: the port describes
 * a mapping only the naming client's own NAT knows about. What it does check is the
 * same thing it checks for a CTM - that both ends are logged in.
 */
EXO_TEST(adchub_a_nat_traversal_request_reaches_the_other_client,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(31), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(32), "bob"));

	bob.drain();
	bob.received.clear();

	/* Alice names the port she will dial from. */
	alice.send("DNAT " + alice.sid + " " + bob.sid + " ADC/1.0 1000 tok1");

	std::string line;
	EXO_ASSERT(bob.waitFor("DNAT", line));
	EXO_ASSERT(line.find(alice.sid) != std::string::npos);
	EXO_ASSERT(line.find("1000") != std::string::npos);
	EXO_ASSERT(line.find("tok1") != std::string::npos);
	return 1;
});

/* And the answer comes back the same way. */
EXO_TEST(adchub_a_nat_traversal_answer_reaches_the_other_client,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(33), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(34), "bob"));

	alice.drain();
	alice.received.clear();

	bob.send("DRNT " + bob.sid + " " + alice.sid + " ADC/1.0 2000 tok1");

	std::string line;
	EXO_ASSERT(alice.waitFor("DRNT", line));
	EXO_ASSERT(line.find(bob.sid) != std::string::npos);
	EXO_ASSERT(line.find("2000") != std::string::npos);
	return 1;
});

/* A request naming somebody who is not here reaches nobody. */
EXO_TEST(adchub_a_nat_traversal_request_for_nobody_is_dropped,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(35), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(36), "bob"));

	bob.drain();
	bob.received.clear();

	alice.send("DNAT " + alice.sid + " ZZZZ ADC/1.0 1000 tok1");

	pump([] { return false; }, 300);
	bob.drain();
	EXO_ASSERT(bob.received.find("DNAT") == std::string::npos);
	return 1;
});

/*
 * And the hub says it will relay them, so a client knows before it tries. Signalled
 * by the hub because relaying NATT and being able to use it are different claims.
 */
EXO_TEST(adchub_the_hub_says_it_relays_nat_traversal,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));

	alice.send("HSUP ADBASE ADTIGR");

	std::string line;
	EXO_ASSERT(alice.waitFor("ISUP", line));
	EXO_ASSERT(line.find("ADNATT") != std::string::npos);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Checking a reachability claim                                             */
/* ------------------------------------------------------------------------- */

/*
 * A client announcing TCP4 asserts something every other client acts on: that a peer
 * told to connect to it will get through. Nothing in ADC checks it, and a client that
 * is wrong fails every transfer - worse, since NATT engages only when neither side
 * claims TCP4, so a false claim also suppresses the fallback that would have worked.
 *
 * The hub can check, because it has the client's address without being told and the
 * client's own CTM names the port. What it must not become is a way to have a hub
 * connect wherever it is asked, which is what most of the cases below are about.
 *
 * The connection made here is a real one, to a real loopback port, so these are as
 * close to the live behaviour as a single machine gets.
 */

/* Somewhere a connection can be made, and somewhere it cannot. */
class Listener : public Samurai::IO::Net::ServerSocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		size_t accepted = 0;

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			return server && server->getFD() != -1 && server->listen();
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }

		/** The port this had, given up - so nothing is listening on it now. */
		uint16_t release()
		{
			const uint16_t was = port();
			server.reset();
			return was;
		}

		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }
		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket>) override { accepted++; }
};

/* The status a client is sent when its claim did not hold. */
static const char CLAIM_FAILED[] = "ISTA 142";

/*
 * Nothing there: the hub says so, naming the port in a field rather than only in the
 * text, so a client can act on it without reading English.
 */
EXO_TEST(adchub_a_claim_that_nothing_answers_is_reported,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	/* A port that was ours and is not any more, so it is free and closed. */
	Listener gone;
	if (!gone.listen()) EXO_SKIP("could not listen on a loopback port");
	const uint16_t closed = gone.release();

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(60), "alice", "SUADC0,TCP4,UDP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(61), "bob"));

	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
		+ std::to_string(closed) + " tok1");

	std::string line;
	EXO_ASSERT(alice.waitFor("ISTA", line));
	EXO_ASSERT(line.compare(0, strlen(CLAIM_FAILED), CLAIM_FAILED) == 0);

	/* The port in the text, and 'PR' meaning what it means: the protocol from
	   the CTM, not a port dressed up in a field that is not for one. */
	EXO_ASSERT(line.find(std::to_string(closed)) != std::string::npos);
	EXO_ASSERT(line.find("PRADC/1.0") != std::string::npos);

	/* No 'TO': this is the hub reporting on a connection it made itself, which is
	   what tells it apart from a peer's failure relayed through. */
	EXO_ASSERT(line.find(" TO") == std::string::npos);
	return 1;
});

/* And the request still goes through: the claim is wrong, not the command. */
EXO_TEST(adchub_a_claim_check_does_not_swallow_the_request,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Listener gone;
	if (!gone.listen()) EXO_SKIP("could not listen on a loopback port");
	const uint16_t closed = gone.release();

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(62), "alice", "SUADC0,TCP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(63), "bob"));

	bob.drain();
	bob.received.clear();

	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
		+ std::to_string(closed) + " tok1");

	std::string line;
	EXO_ASSERT(bob.waitFor("DCTM", line));
	EXO_ASSERT(line.find("tok1") != std::string::npos);
	return 1;
});

/*
 * Something answers: nothing is said. A hub telling a client that its settings are
 * correct is a message nobody needs, and one that says it anyway trains its users to
 * ignore the case where it matters.
 */
EXO_TEST(adchub_a_claim_that_holds_is_not_reported,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Listener open;
	if (!open.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(64), "alice", "SUADC0,TCP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(65), "bob"));

	alice.drain();
	alice.received.clear();

	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
		+ std::to_string(open.port()) + " tok1");

	/* Long enough for the connection to be made and reported on. */
	pump([&] { return open.accepted > 0; }, 2000);
	pump([] { return false; }, 300);

	alice.drain();
	EXO_ASSERT_EQ_UINT(open.accepted, 1u);
	EXO_ASSERT(alice.received.find("ISTA") == std::string::npos);
	return 1;
});

/*
 * A client that claims nothing is not checked. It has not asserted anything, and a
 * hub connecting to it anyway would be making an outbound connection nobody asked
 * for - which is the thing this must not become.
 */
EXO_TEST(adchub_a_client_claiming_nothing_is_not_checked,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Listener open;
	if (!open.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(66), "alice", "SUADC0,UDP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(67), "bob"));

	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
		+ std::to_string(open.port()) + " tok1");

	pump([] { return false; }, 700);
	EXO_ASSERT_EQ_UINT(open.accepted, 0u);
	EXO_ASSERT_EQ_UINT(ADC::LocalHub::ClaimCheck::running(), 0u);
	return 1;
});

/*
 * A privileged port is not where a peer transfer listens, and it is where everything
 * else does. The port is the one thing in this the client chooses, and the address it
 * goes with is shared with whoever else is behind the same NAT - so a client naming
 * 22 must not have the hub connect to its neighbour's ssh.
 */
EXO_TEST(adchub_a_claim_on_a_privileged_port_is_not_checked,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(68), "alice", "SUADC0,TCP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(69), "bob"));

	alice.drain();
	alice.received.clear();

	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 22 tok1");

	pump([] { return false; }, 700);
	EXO_ASSERT_EQ_UINT(ADC::LocalHub::ClaimCheck::running(), 0u);

	/* And no report either: nothing was checked, so nothing is known. */
	alice.drain();
	EXO_ASSERT(alice.received.find("ISTA") == std::string::npos);
	return 1;
});

/* Nor is a port outside the range one at all. */
EXO_TEST(adchub_a_claim_on_an_impossible_port_is_not_checked,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(70), "alice", "SUADC0,TCP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(71), "bob"));

	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 99999 tok1");
	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 0 tok2");
	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 -1 tok3");

	pump([] { return false; }, 700);
	EXO_ASSERT_EQ_UINT(ADC::LocalHub::ClaimCheck::running(), 0u);
	return 1;
});

/*
 * One check per endpoint per interval. A client that asks for a connection every few
 * seconds would otherwise have the hub dial it every time, which is a load the hub
 * did not choose and the client did.
 */
EXO_TEST(adchub_the_same_endpoint_is_checked_once_per_interval,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Listener open;
	if (!open.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(72), "alice", "SUADC0,TCP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(73), "bob"));

	for (int n = 0; n < 6; n++)
	{
		alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
			+ std::to_string(open.port()) + " tok" + std::to_string(n));
		pump([] { return false; }, 150);
	}

	pump([] { return false; }, 500);
	EXO_ASSERT_EQ_UINT(open.accepted, 1u);
	return 1;
});

/* A different port is a different endpoint, and is checked on its own. */
EXO_TEST(adchub_a_different_port_is_a_different_endpoint,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Listener first;
	Listener second;
	if (!first.listen() || !second.listen())
		EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(74), "alice", "SUADC0,TCP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(75), "bob"));

	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
		+ std::to_string(first.port()) + " tok1");
	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
		+ std::to_string(second.port()) + " tok2");

	pump([&] { return first.accepted && second.accepted; }, 2000);
	EXO_ASSERT_EQ_UINT(first.accepted, 1u);
	EXO_ASSERT_EQ_UINT(second.accepted, 1u);
	return 1;
});

/* An operator who wants the hub to make no outbound connections can say so. */
EXO_TEST(adchub_an_operator_can_turn_claim_checking_off,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	s.hub.checkClaims = false;

	Listener open;
	if (!open.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(76), "alice", "SUADC0,TCP4"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(77), "bob"));

	alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
		+ std::to_string(open.port()) + " tok1");

	pump([] { return false; }, 700);
	EXO_ASSERT_EQ_UINT(open.accepted, 0u);

	/* The request itself is unaffected. */
	std::string line;
	EXO_ASSERT(bob.sawLine({"DCTM", "tok1"}) || bob.waitFor("DCTM", line));
	return 1;
});

/* On by default, which is the whole point: a claim nobody checks is a claim. */
EXO_TEST(adchub_claim_checking_is_on_unless_it_is_turned_off,
{
	QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
	config->setGroup("Local hub");
	config->remove("Check reachability claims");

	Server s;
	s.hub.checkClaims = false;
	s.hub.readConfiguration();

	EXO_ASSERT(s.hub.checkClaims);
	return 1;
});

/*
 * A client that goes away while it is being checked is not one to report to. The
 * check outlives it - it is looking at a closed port and takes seconds to find out -
 * so the user it names is looked up when there is something to say, not remembered.
 */
EXO_TEST(adchub_a_client_that_leaves_during_a_check_is_not_reported_to,
{
	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Listener gone;
	if (!gone.listen()) EXO_SKIP("could not listen on a loopback port");
	const uint16_t closed = gone.release();

	Client bob;
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(78), "bob"));

	{
		Client alice;
		EXO_ASSERT(alice.connect(s.port()));
		EXO_ASSERT(alice.login(makeIdentity(79), "alice", "SUADC0,TCP4"));

		alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
			+ std::to_string(closed) + " tok1");

		pump([] { return ADC::LocalHub::ClaimCheck::running() > 0; }, 1000);
		EXO_ASSERT_EQ_UINT(ADC::LocalHub::ClaimCheck::running(), 1u);
	}

	/* Alice's socket is gone; the check finishes and finds nobody. */
	pump([] { return ADC::LocalHub::ClaimCheck::running() == 0; }, 3000);
	EXO_ASSERT_EQ_UINT(ADC::LocalHub::ClaimCheck::running(), 0u);
	return 1;
});

/*
 * And a hub that goes away while a check is running takes the check with it: the
 * check holds the hub it will report to, so one left behind reports into freed
 * memory. Nothing to assert beyond finishing without a fault, which under the
 * sanitizer is the assertion.
 */
EXO_TEST(adchub_a_hub_that_goes_away_takes_its_checks_with_it,
{
	Listener gone;
	if (!gone.listen()) EXO_SKIP("could not listen on a loopback port");
	const uint16_t closed = gone.release();

	{
		Server s;
		if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

		Client alice;
		Client bob;
		EXO_ASSERT(alice.connect(s.port()));
		EXO_ASSERT(alice.login(makeIdentity(80), "alice", "SUADC0,TCP4"));
		EXO_ASSERT(bob.connect(s.port()));
		EXO_ASSERT(bob.login(makeIdentity(81), "bob"));

		alice.send("DCTM " + alice.sid + " " + bob.sid + " ADC/1.0 "
			+ std::to_string(closed) + " tok1");

		pump([] { return ADC::LocalHub::ClaimCheck::running() > 0; }, 1000);
		EXO_ASSERT_EQ_UINT(ADC::LocalHub::ClaimCheck::running(), 1u);
	}

	EXO_ASSERT_EQ_UINT(ADC::LocalHub::ClaimCheck::running(), 0u);

	pump([] { return false; }, 1500);
	return 1;
});
