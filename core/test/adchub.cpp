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
#include <vector>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)


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

/* How many arguments one command may carry. */
static void test_argument_ceiling()
{
	/* As many as are allowed, counting the source ID the 'B' prefix consumes. */
	std::string ok = "BINF AAAA";
	for (size_t n = 1; n < ADC::MAX_COMMAND_ARGUMENTS; n++) ok += " AA1";

	ADC::Command accepted;
	CHECK(ADC::Parser::parse(ok.c_str(), ok.size(), accepted));
	CHECK(accepted.countArguments() == ADC::MAX_COMMAND_ARGUMENTS - 1);

	const std::string one_too_many = ok + " AA1";
	ADC::Command refused;
	CHECK(!ADC::Parser::parse(one_too_many.c_str(), one_too_many.size(), refused));

	/*
	 * And the case it exists for: a line of the length one packet can hold,
	 * entirely made of INF arguments. Each of these used to have the whole
	 * accumulated INF deep copied for it, and the copy leaked.
	 */
	std::string flood = "BINF AAAA";
	while (flood.size() < 128 * 1024) flood += " AA1";

	ADC::Command bomb;
	CHECK(!ADC::Parser::parse(flood.c_str(), flood.size(), bomb));

	printf("  ok a command carries at most %zu arguments\n", ADC::MAX_COMMAND_ARGUMENTS);
}

/* What the send queue may hold before it refuses. */
static void test_send_queue_ceiling()
{
	QuickDC::CommandFIFO fifo;

	/* No ceiling unless one is asked for, which is what the client sessions
	   sharing this queue expect. */
	CHECK(fifo.getMaxQueueSize() == 0);

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

	CHECK(queued > 0);
	CHECK(refused > 0);
	CHECK(queued + refused == 1000);
	CHECK(fifo.getQueueSize() <= 1024);

	printf("  ok the send queue refuses past its ceiling (%zu queued, %zu refused)\n",
		queued, refused);
}

/* ------------------------------------------------------------------------- */

/* A line that never ends. */
static void test_unterminated_line()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	Client c;
	CHECK(c.connect(s.port()));
	CHECK(pump([&] { return s.hub.users.size() == 1; }));

	/* Enough to be recognised, and then nothing that ends a line. */
	c.send("HSUP ADBASE ADTIGR");

	std::string line;
	CHECK(c.waitFor("ISID", line));

	c.sendRaw(std::string(32 * 1024, 'A'));

	CHECK(pump([&] { return c.disconnected; }));
	CHECK(pump([&] { return s.hub.users.empty(); }));
	printf("  ok a line that is never terminated closes the connection\n");
}

/* The features a user may advertise, and what happens when it advertises none. */
static void test_feature_set()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	Client c;
	CHECK(c.connect(s.port()));
	CHECK(pump([&] { return s.hub.users.size() == 1; }));
	if (s.hub.users.empty()) { failures++; return; }

	ADC::HubUser* user = s.hub.users[0];

	/* A null feature is not a feature, and asking must not read through it. */
	CHECK(!user->hasFeature(0));
	CHECK(!user->hasFeature("TOOLONG"));
	CHECK(!user->hasFeature("AB"));

	user->clearFeatures();
	user->addFeatures("TIGR,ADCS,BZIP");
	CHECK(user->countFeatures() == 3);
	CHECK(user->hasFeature("TIGR"));
	CHECK(user->hasFeature("ADCS"));
	CHECK(user->hasFeature("BZIP"));
	CHECK(!user->hasFeature("SEGA"));

	/* And the field as an attacker writes it. */
	user->clearFeatures();
	std::string many;
	while (many.size() < 128 * 1024) many += "AA1,";
	user->addFeatures(many.c_str());
	CHECK(user->countFeatures() <= ADC::MAX_USER_FEATURES);

	printf("  ok a user advertises at most %zu features\n", ADC::MAX_USER_FEATURES);
}

/* Every SID the hub hands out has to be one it can find again. */
static void test_session_ids()
{
	Server s;
	if (!s.listen()) { failures++; return; }

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
		CHECK(c->connect(s.port()));
	}

	CHECK(pump([&] { return s.hub.users.size() == clients.size(); }));
	CHECK(s.hub.sid_map.size() == s.hub.users.size());

	for (size_t n = 0; n < s.hub.users.size(); n++)
	{
		const sid_t sid = s.hub.users[n]->getSID();

		CHECK(sid != 0);
		CHECK(sid == (sid & ADC::SID_MASK));

		/* The route a D or an E message takes to find its target. */
		CHECK(s.hub.lookupUser(ADC::SID::fromString(ADC::SID::toString(sid)))
			== s.hub.users[n]);
	}

	for (size_t n = 0; n < clients.size(); n++) delete clients[n];
	printf("  ok every session ID handed out resolves back to its user\n");
}

/* One packet faults that a logged in user can send. */
static void test_malformed_commands()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	Client c;
	CHECK(c.connect(s.port()));
	if (!c.connected) { failures++; return; }
	CHECK(c.login(makeIdentity(1), "tester"));

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
	CHECK(c.waitFor("BMSG", line));
	CHECK(line.find("still\\salive") != std::string::npos);
	CHECK(s.hub.users.size() == 1);
	printf("  ok a message with no message does not take the hub down\n");
}

/* An INF that is updated over and over. */
static void test_inf_updates()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	Client alice;
	CHECK(alice.connect(s.port()));
	CHECK(alice.login(makeIdentity(8), "alice"));

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
		CHECK(alice.waitFor("BINF", line));
	}

	Client bob;
	CHECK(bob.connect(s.port()));
	CHECK(bob.login(makeIdentity(9), "bob"));

	/* Everything alice ever said about herself, in one command. */
	CHECK(bob.sawLine({"BINF", alice.sid, "NIalice", "DEround49", "SS49"}));
	printf("  ok an INF is accumulated across updates and handed on whole\n");
}

/* A user that claims a session ID that is not its own. */
static void test_sid_mismatch()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	Client alice;
	Client bob;
	CHECK(alice.connect(s.port()));
	CHECK(alice.login(makeIdentity(2), "alice"));
	CHECK(bob.connect(s.port()));
	CHECK(bob.login(makeIdentity(3), "bob"));

	/* A source SID naming nobody, which is what leaves the hub with no user to
	   name in what it tells everyone else. */
	bob.send("DMSG ZZZZ " + alice.sid + " a\\sword\\sfrom\\snobody");

	std::string line;
	CHECK(bob.waitFor("ISTA", line));
	CHECK(line.find("240") != std::string::npos);

	/*
	 * And the others are told bob is gone, by bob's own SID. Without it every
	 * remaining user keeps an entry for a user that is no longer there.
	 */
	CHECK(alice.waitFor("IQUI", line));
	CHECK(line.find(bob.sid) != std::string::npos);

	CHECK(pump([&] { return s.hub.users.size() == 1; }));
	printf("  ok a mismatched source SID retires the connection and says whose it was\n");
}

/* How many connections, and from where. */
static void test_admission_control()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	s.hub.maxPerAddress = 3;

	std::vector<Client*> clients;
	for (int n = 0; n < 6; n++)
	{
		Client* c = new Client();
		clients.push_back(c);
		c->connect(s.port());
	}

	/* Every one of them was accepted by the kernel; only three became users. */
	CHECK(pump([&] { return s.accepted == clients.size(); }));
	CHECK(pump([&] { return s.hub.users.size() == 3; }, 1000));
	CHECK(s.hub.users.size() == 3);

	for (size_t n = 0; n < clients.size(); n++) delete clients[n];

	printf("  ok connections past the per address limit do not become users\n");
}

static void test_handshake_limit()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	s.hub.maxPerAddress = 64;
	s.hub.maxHandshake = 2;

	std::vector<Client*> clients;
	for (int n = 0; n < 5; n++)
	{
		Client* c = new Client();
		clients.push_back(c);
		c->connect(s.port());
	}

	CHECK(pump([&] { return s.accepted == clients.size(); }));
	CHECK(pump([&] { return s.hub.users.size() == 2; }, 1000));
	CHECK(s.hub.countHandshaking() == 2);

	for (size_t n = 0; n < clients.size(); n++) delete clients[n];

	printf("  ok connections past the handshake limit do not become users\n");
}

/* A connection that says the four bytes the probe wants, and then nothing. */
static void test_handshake_timeout()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	s.hub.handshakeTimeout = 1;
	s.sweepEvery(std::chrono::milliseconds(50));

	Client c;
	CHECK(c.connect(s.port()));
	CHECK(pump([&] { return s.hub.users.size() == 1; }));

	/* Nothing else is ever sent. */
	CHECK(pump([&] { return c.disconnected; }, 6000));
	CHECK(pump([&] { return s.hub.users.empty(); }));
	printf("  ok a connection that never logs in is swept away\n");
}

/* And one that logs in and then goes quiet. */
static void test_idle_timeout()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	s.hub.handshakeTimeout = 0;
	s.hub.idleTimeout = 1;
	s.sweepEvery(std::chrono::milliseconds(50));

	Client c;
	CHECK(c.connect(s.port()));
	CHECK(c.login(makeIdentity(4), "quiet"));

	CHECK(pump([&] { return c.disconnected; }, 6000));
	CHECK(pump([&] { return s.hub.users.empty(); }));
	printf("  ok a logged in user that goes quiet is swept away\n");
}

/* A hub that is full, and who is let in anyway. */
static void test_user_limit()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	s.hub.maxUsers = 1;

	/*
	 * Both of these connect from the loopback address, which the hub gives
	 * administrator credentials - so both are exempt from the limit. Written as
	 * an 'or' the exemption held for everyone, which is to say for nobody.
	 */
	Client first;
	Client second;
	CHECK(first.connect(s.port()));
	CHECK(first.login(makeIdentity(5), "first"));
	CHECK(second.connect(s.port()));
	CHECK(second.login(makeIdentity(6), "second"));

	CHECK(s.hub.countLoggedIn() == 2);
	printf("  ok an operator is let into a full hub\n");
}

/* What the chat history keeps. */
static void test_chat_history()
{
	Server s;
	if (!s.listen()) { failures++; return; }

	Client c;
	CHECK(c.connect(s.port()));
	CHECK(c.login(makeIdentity(7), "talker"));
	if (s.hub.users.empty()) { failures++; return; }

	ADC::HubUser* user = s.hub.users[0];

	/* A message with nothing in it is not one. */
	s.hub.onPublicChat(user, 0);
	CHECK(s.hub.history.empty());

	s.hub.maxHistory = 3;
	for (int n = 0; n < 20; n++) s.hub.onPublicChat(user, "something");
	CHECK(s.hub.history.size() == 3);

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
	CHECK(s.hub.history.empty());

	printf("  ok the chat history keeps what it is configured to\n");
}

/* Every configured limit is one the hub can work with. */
static void test_configuration_clamping()
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
		CHECK(hub.maxUsers >= 1);
		CHECK(hub.maxHistory == 0);
		CHECK(hub.maxSendQ >= 4096);
		CHECK(hub.hardSendQ > hub.maxSendQ);
		CHECK(hub.maxPerAddress >= 1);
		CHECK(hub.maxHandshake >= 1);
		CHECK(hub.handshakeTimeout == 0);
		CHECK(hub.idleTimeout == 0);
	}

	config->setGroup("Local hub");
	config->remove("Max users");
	config->remove("Max history");
	config->remove("Max send queue bytes");
	config->remove("Max connections per address");
	config->remove("Max connections in handshake");
	config->remove("Handshake timeout");
	config->remove("Idle timeout");

	printf("  ok a configured limit that makes no sense is clamped\n");
}

/* Users come and go without leaving anything in the socket monitor. */
static void test_no_leftovers()
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();
	const size_t baseline = monitor->size();

	for (int n = 0; n < 3; n++)
	{
		Server s;
		if (!s.listen()) { failures++; return; }

		Client c;
		CHECK(c.connect(s.port()));
		CHECK(c.login(makeIdentity((uint8_t) (10 + n)), "comer"));
	}

	pump([&] { return monitor->size() <= baseline; }, 2000);
	CHECK(monitor->size() == baseline);
	printf("  ok hubs come and go leaving nothing behind\n");
}

int main()
{
	setbuf(stdout, 0);

	const std::string cfg = "/tmp/quickdc_adchubtest_config.conf";
	const std::string ses = "/tmp/quickdc_adchubtest_session.conf";

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	{
		/* Scoped, because Preferences writes itself out when it is destroyed. */
		QuickDC::Core core(cfg, ses);

		test_argument_ceiling();
		test_send_queue_ceiling();
		test_unterminated_line();
		test_feature_set();
		test_session_ids();
		test_malformed_commands();
		test_inf_updates();
		test_sid_mismatch();
		test_admission_control();
		test_handshake_limit();
		test_handshake_timeout();
		test_idle_timeout();
		test_user_limit();
		test_chat_history();
		test_configuration_clamping();
		test_no_leftovers();
	}

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
