/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The NMDC counterpart of hubsession.cpp: a real DC::HubSession talking to a
 * hub that only exists for the test, over a loopback port the kernel picks.
 *
 * NMDC differs from ADC in the two ways that matter here. Commands end with
 * '|' rather than a newline, and the hub speaks first - it opens with $Lock,
 * and the client has to answer with a key derived from it before anything else
 * happens. So the login exchange runs the other way round from the ADC one.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/hub.h"
#include "network/dc/hubsession.h"
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

/* A lock long enough to derive a key from, as a real hub sends. */
#define HUB_LOCK "EXTENDEDPROTOCOLABCABCABCABCABCABC"

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

/* Records what the session told the interface. */
class Listener : public QuickDC::HubListener
{
	public:
		std::vector<std::string> chat;
		std::vector<std::string> privateChat;
		std::vector<std::string> hubMessages;
		std::vector<std::string> joined;
		std::vector<std::string> left;
		std::vector<std::string> errors;
		std::string hubName;
		int netConnected = 0;
		bool authRequested = false;

		void EventNetStatus(enum StatusNetwork state) override
		{ if (state == NetConnected) netConnected++; }

		void EventHubStatus(enum StatusHub) override { }

		void EventSystemError(const char* msg) override
		{ errors.push_back(msg ? msg : ""); }

		void EventChat(const QuickDC::User*, const char* message, bool) override
		{ chat.push_back(message ? message : ""); }

		void EventPrivateChat(const QuickDC::User*, const QuickDC::User*,
			const char* message, const char*, bool) override
		{ privateChat.push_back(message ? message : ""); }

		void EventHubMessage(const char*, const char* message) override
		{ hubMessages.push_back(message ? message : ""); }

		void EventUserJoin(const char* user) override
		{ joined.push_back(user ? user : ""); }

		void EventUserLeave(const QuickDC::User* user, const char*, bool) override
		{ left.push_back(user && user->getNick() ? user->getNick() : ""); }

		void EventUsersCleanup() override { }
		void EventUserUpdate(const QuickDC::User*) override { }
		void EventSearchResult(void*) override { }

		void EventHubName(const char* name) override
		{ hubName = name ? name : ""; }

		bool EventHubRedirect(const char*, uint16_t) override { return false; }
		void EventHubAutenticate() override { authRequested = true; }

		bool EventClientConnect(const char*, uint16_t, const QuickDC::User*) override
		{ return false; }
		bool EventClientConnect(const QuickDC::User*) override { return false; }
};

/* The other end: a listening socket and whatever the client sent. */
class FakeHub :
	public Samurai::IO::Net::ServerSocketEventHandler,
	public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> peer;
		std::string received;

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			return server && server->getFD() != -1 && server->listen();
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }

		bool accepted() { return pump([&] { return peer != 0; }); }

		/* NMDC ends every command with '|'. */
		void send(const std::string& command)
		{
			if (!peer) return;
			const std::string out = command + "|";
			peer->write(out.data(), out.size());
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

		/* Reads commands until one starts with 'prefix', or gives up. */
		bool expect(const char* prefix, std::string& out)
		{
			for (int n = 0; n < 20; n++)
			{
				if (!nextCommand(out)) return false;
				if (out.compare(0, strlen(prefix), prefix) == 0) return true;
			}
			return false;
		}

		void drain()
		{
			if (!peer) return;
			for (;;)
			{
				char buf[4096];
				size_t got = 0;
				std::error_code ec;
				if (peer->read(buf, sizeof(buf), got, ec) != Samurai::IO::ReadResult::Ok) break;
				if (!got) break;
				received.append(buf, got);
			}
		}

		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			peer = socket;
			peer->setEventHandler(this);
		}

		void EventHostLookup(const Samurai::IO::Net::Socket*) override { }
		void EventHostFound(const Samurai::IO::Net::Socket*) override { }
		void EventConnecting(const Samurai::IO::Net::Socket*) override { }
		void EventConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTimeout(const Samurai::IO::Net::Socket*) override { }
		void EventDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { }
		void EventCanWrite(const Samurai::IO::Net::Socket*) override { }
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override { }
};

/* A hub and a session connected to it. */
class Session
{
	public:
		FakeHub hub;
		Listener listener;
		std::unique_ptr<Samurai::IO::Net::URL> url;
		std::unique_ptr<QuickDC::Hub> ui;
		DC::HubSession* session = 0;
		std::string nick;
		bool ready = false;

		Session()
		{
			if (!hub.listen()) return;

			const std::string address =
				"dchub://127.0.0.1:" + std::to_string(hub.port());
			url = std::make_unique<Samurai::IO::Net::URL>(address);

			ui = std::make_unique<QuickDC::Hub>(&listener, url.get());
			session = new DC::HubSession(ui.get(), &listener, url.get());

			session->connect();
			if (!hub.accepted()) return;

			ready = true;
		}

		~Session()
		{
			delete session;
			ui.reset();
		}

		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		void say(const std::string& command)
		{
			hub.send(command);
			pump([] { return false; }, 60);
		}

		/*
		 * The hub speaks first. The client answers the lock with a key and its
		 * nick, and the hub confirms with $Hello.
		 */
		bool login()
		{
			say("$Lock " HUB_LOCK " Pk=TESTHUB");

			std::string line;
			if (!hub.expect("$Key ", line)) return false;
			if (!hub.expect("$ValidateNick ", line)) return false;

			nick = line.substr(strlen("$ValidateNick "));
			if (nick.empty()) return false;

			say("$Hello " + nick);
			say("$HubName TestHub");

			/* Its own $Hello is what makes the client send its info. */
			if (!hub.expect("$MyINFO ", line)) return false;

			/* The hub broadcasts it back, which is what completes the login. */
			say("$MyINFO $ALL " + nick + " $ $DSL\x01$$0$");
			return true;
		}
};

/* ------------------------------------------------------------------------- */

static void run()
{
	{
		Session s;
		CHECK(s.ready);
		CHECK(s.listener.netConnected == 1);
		printf("  ok the session connects to the hub\n");
	}

	/* The hub speaks first, and the client answers the lock with a key. */
	{
		Session s;
		if (s.ready)
		{
			s.say("$Lock " HUB_LOCK " Pk=TESTHUB");

			std::string line;
			CHECK(s.hub.expect("$Key ", line));
			CHECK(line.size() > strlen("$Key "));
			printf("  ok a lock is answered with a key\n");
		}
		else failures++;
	}

	/* An extended-protocol lock also gets $Supports. */
	{
		Session s;
		if (s.ready)
		{
			s.say("$Lock " HUB_LOCK " Pk=TESTHUB");

			std::string line;
			CHECK(s.hub.expect("$Supports ", line));
			printf("  ok an extended lock is answered with $Supports: %s\n", line.c_str());
		}
		else failures++;
	}

	/* And then the nick it wants. */
	{
		Session s;
		if (s.ready)
		{
			s.say("$Lock " HUB_LOCK " Pk=TESTHUB");

			std::string line;
			CHECK(s.hub.expect("$ValidateNick ", line));
			CHECK(line.size() > strlen("$ValidateNick "));
			printf("  ok it asks to validate a nick: %s\n", line.c_str());
		}
		else failures++;
	}

	/*
	 * A lock too short to derive a key from arrives before anything has been
	 * verified, and the connection is dropped rather than answered.
	 */
	{
		Session s;
		if (s.ready)
		{
			s.say("$Lock ab Pk=x");
			pump([] { return false; }, 200);

			s.hub.drain();
			CHECK(s.hub.received.find("$Key") == std::string::npos);
			printf("  ok a lock too short to answer is refused\n");
		}
		else failures++;
	}

	/* $Hello for our own nick is what asks for our info. */
	{
		Session s;
		if (s.ready && s.login())
		{
			printf("  ok $Hello is answered with $MyINFO\n");
		}
		else failures++;
	}

	/* ---- who is on the hub ---- */

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$Hello alice");

			QuickDC::UserManager* users = s.session->getUserManager();
			CHECK(users != 0);
			CHECK(users && users->getUser("alice") != 0);
			printf("  ok $Hello for someone else puts them on the hub\n");
		}
		else failures++;
	}

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$MyINFO $ALL alice a description$ $DSL\x01$mail@example$1024$");

			QuickDC::UserManager* users = s.session->getUserManager();
			const QuickDC::User* alice = users ? users->getUser("alice") : 0;

			CHECK(alice != 0);
			CHECK(alice && alice->getSharedBytes() == 1024);
			CHECK(users && users->countShared() == 1024);
			printf("  ok $MyINFO carries the share size\n");
		}
		else failures++;
	}

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
			s.say("$MyINFO $ALL bob y$ $DSL\x01$m@e$200$");

			QuickDC::UserManager* users = s.session->getUserManager();
			CHECK(users && users->countShared() == 300);

			s.say("$Quit alice");
			CHECK(users && users->getUser("alice") == 0);
			CHECK(users && users->getUser("bob") != 0);
			CHECK(users && users->countShared() == 200);
			printf("  ok $Quit takes the user and their share away\n");
		}
		else failures++;
	}

	/* A second $MyINFO for the same nick is an update, not a new user. */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");

			QuickDC::UserManager* users = s.session->getUserManager();
			const size_t before = users ? users->countUsers() : 0;

			s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$500$");

			CHECK(users && users->countUsers() == before);
			const QuickDC::User* alice = users ? users->getUser("alice") : 0;
			CHECK(alice && alice->getSharedBytes() == 500);
			printf("  ok a second $MyINFO updates rather than duplicates\n");
		}
		else failures++;
	}

	/*
	 * $NickList is only a list of names. Unless the hub said NoGetINFO, the
	 * client asks after each of them, and they arrive as $MyINFO.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$NickList alice$$bob$$carol$$");

			std::string line;
			CHECK(s.hub.expect("$GetINFO alice ", line));
			CHECK(s.hub.expect("$GetINFO bob ", line));
			CHECK(s.hub.expect("$GetINFO carol ", line));

			/* Nobody is on the hub until the replies arrive. */
			QuickDC::UserManager* users = s.session->getUserManager();
			CHECK(users && users->getUser("alice") == 0);

			s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
			CHECK(users && users->getUser("alice") != 0);
			printf("  ok $NickList is followed up with $GetINFO for each nick\n");
		}
		else failures++;
	}

	/* ---- messages ---- */

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$Hello alice");
			s.say("<alice> hello everyone");

			CHECK(s.listener.chat.size() == 1);
			CHECK(!s.listener.chat.empty() && s.listener.chat[0] == "hello everyone");
			printf("  ok public chat is attributed and reported\n");
		}
		else failures++;
	}

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$Hello alice");
			s.say("$To: " + s.nick + " From: alice $<alice> a private word");

			CHECK(s.listener.privateChat.size() == 1);
			CHECK(!s.listener.privateChat.empty()
				&& s.listener.privateChat[0].find("a private word") != std::string::npos);
			printf("  ok a private message is reported separately\n");
		}
		else failures++;
	}

	{
		Session s;
		if (s.ready)
		{
			s.say("$Lock " HUB_LOCK " Pk=TESTHUB");
			s.say("$HubName My Hub");

			CHECK(s.listener.hubName == "My Hub");
			printf("  ok the hub's name reaches the interface\n");
		}
		else failures++;
	}

	/* ---- what a hub can throw at a client ---- */

	{
		Session s;
		if (s.ready && s.login())
		{
			/* Chat from, and a quit for, someone who is not here. */
			s.say("<nobody> talking");
			s.say("$Quit nobody");

			/* Commands with nothing after the keyword. */
			s.say("$Hello");
			s.say("$Quit");
			s.say("$MyINFO");
			s.say("$NickList");
			s.say("$HubName");
			s.say("$Lock");
			s.say("$To:");

			/* A $MyINFO missing most of its fields. */
			s.say("$MyINFO $ALL truncated");
			s.say("$MyINFO $ALL alice x$ $DSL");

			/* Not NMDC at all. */
			s.say("");
			s.say("$NoSuchCommand with arguments");
			s.say("plain text with no nick");

			QuickDC::UserManager* users = s.session->getUserManager();
			CHECK(users != 0);
			printf("  ok malformed and unknown commands are survived\n");
		}
		else failures++;
	}

	/*
	 * A password request is answered from what was configured, without asking
	 * anyone. HubListener declares EventHubAutenticate for this and nothing in
	 * the tree ever calls it, so an unconfigured password sends an empty one
	 * rather than prompting.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$GetPass");

			std::string line;
			CHECK(s.hub.expect("$MyPass", line));
			CHECK(!s.listener.authRequested);
			printf("  ok a password request is answered from the configuration\n");
		}
		else failures++;
	}

	{
		Session s;
		if (s.ready && s.login())
		{
			CHECK(s.session->isLoggedIn());

			s.hub.peer.reset();
			CHECK(pump([&] { return !s.session->isLoggedIn(); }));
			printf("  ok the session notices the hub going away\n");
		}
		else failures++;
	}

	/* Sessions come and go without leaving anything in the monitor. */
	{
		Samurai::IO::Net::SocketMonitor* monitor =
			Samurai::IO::Net::SocketMonitor::getInstance();
		const size_t baseline = monitor->size();

		for (int n = 0; n < 3; n++)
		{
			Session s;
			CHECK(s.ready);
			if (s.ready) s.login();
		}

		pump([&] { return monitor->size() <= baseline; }, 2000);
		CHECK(monitor->size() == baseline);
		printf("  ok repeated sessions leave nothing behind\n");
	}
}

int main()
{
	setbuf(stdout, 0);

	const std::string cfg = "/tmp/quickdc_dchubtest_config.conf";
	const std::string ses = "/tmp/quickdc_dchubtest_session.conf";

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	{
		/* Scoped: Preferences writes itself out when destroyed, so the files
		   can only be removed once the core holding them is gone. */
		QuickDC::Core core(cfg, ses);
		run();
	}

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
