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
#include "network/dc/dccommandparser.h"
#include "network/dc/dccommandproxy.h"
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

/* Where the core under test keeps its session data. */
static const char* SESSION_FILE = "/tmp/quickdc_dchubtest_session.conf";

static std::string readFile(const char* path)
{
	FILE* fh = fopen(path, "rb");
	if (!fh) return std::string();

	std::string out;
	char buf[4096];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), fh)) > 0) out.append(buf, got);
	fclose(fh);
	return out;
}

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

		/* What a $ConnectToMe, a $ForceMove and a $UserIP asked for. */
		std::vector<std::string> connectTo;
		std::vector<std::string> redirects;
		int connectToKnown = 0;
		int passiveConnect = 0;
		int userUpdates = 0;

		void EventNetStatus(enum StatusNetwork state) override
		{ if (state == NetConnected) netConnected++; }

		void EventHubStatus(enum StatusHub) override { }

		void EventSystemError(const char* msg) override
		{ errors.push_back(msg ? msg : ""); }

		void EventChat(const QuickDC::User*, const char* message, bool, bool) override
		{ chat.push_back(message ? message : ""); }

		void EventPrivateChat(const QuickDC::User*, const QuickDC::User*,
			const char* message, const char*, bool, bool) override
		{ privateChat.push_back(message ? message : ""); }

		void EventHubMessage(const char*, const char* message) override
		{ hubMessages.push_back(message ? message : ""); }

		void EventUserJoin(const char* user) override
		{ joined.push_back(user ? user : ""); }

		void EventUserLeave(const QuickDC::User* user, const char*, bool) override
		{ left.push_back(user && user->getNick() ? user->getNick() : ""); }

		void EventUsersCleanup() override { }
		void EventUserUpdate(const QuickDC::User*) override { userUpdates++; }
		void EventSearchResult(void*) override { }

		void EventHubName(const char* name) override
		{ hubName = name ? name : ""; }

		bool EventHubRedirect(const char* host, uint16_t port) override
		{
			redirects.push_back(std::string(host ? host : "")
				+ ":" + std::to_string(port));
			return false;
		}

		void EventHubAutenticate() override { authRequested = true; }

		/*
		 * Refusing both keeps every case here off the network: the session only
		 * dials out when the interface agrees to it.
		 */
		bool EventClientConnect(const char* address, uint16_t port,
			const QuickDC::User* user) override
		{
			connectTo.push_back(std::string(address ? address : "")
				+ ":" + std::to_string(port));
			if (user) connectToKnown++;
			return false;
		}

		bool EventClientConnect(const QuickDC::User*) override
		{ passiveConnect++; return false; }
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

/*
 * One command handler, called with a line and the length of it and nothing
 * else.
 *
 * The handlers are what a hub reaches before it has authenticated anything, and
 * they are handed the length of the line precisely so that they need not guess
 * where it ends. Calling one directly is what makes that boundary visible: the
 * buffer below is exactly as long as the length says and carries no terminator,
 * so a handler that indexes a fixed offset into it reads memory that is not
 * part of the line rather than a stray byte that happens to be there.
 *
 * A proxy owns one handler per command name and is the only way to reach them,
 * so it is subclassed rather than driven.
 */
class Handlers : public DC::CommandProxy
{
	public:
		explicit Handlers(Session& s) : DC::CommandProxy(s.hub.peer, s.session) { }

		bool call(const char* name, const std::string& line)
		{
			const auto it = mapper.find(name);
			if (it == mapper.end())
			{
				printf("FAIL %s:%d no handler for %s\n", __FILE__, __LINE__, name);
				failures++;
				return false;
			}

			const std::vector<char> exact(line.begin(), line.end());
			return it->second->invoke(exact.data(), exact.size());
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
	 * Nothing here configures a password, so the request is put to the
	 * interface - and answered anyway, with the empty one. The hub refuses
	 * that, which is a faster and clearer end than waiting for an interface
	 * that may never answer.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$GetPass");
			CHECK(s.listener.authRequested);

			std::string line;
			CHECK(s.hub.expect("$MyPass", line));
			printf("  ok a password request with none configured is reported and answered\n");
		}
		else failures++;
	}

	/* A password supplied while the request is still open is sent. */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("$GetPass");

			std::string line;
			CHECK(s.hub.expect("$MyPass", line));

			s.session->sendPassword("hunter2");

			CHECK(s.hub.expect("$MyPass ", line));
			CHECK(line == "$MyPass hunter2");
			printf("  ok a password supplied afterwards is sent\n");
		}
		else failures++;
	}

	/*
	 * A password the user supplied is remembered, so the next run does not ask
	 * again. Written when it is supplied rather than when the session ends, so
	 * that a crash does not lose it.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			std::string line;
			s.say("$GetPass");
			CHECK(s.hub.expect("$MyPass", line));

			s.session->sendPassword("remembered-nmdc");

			/* On disk before the session goes anywhere. */
			CHECK(readFile(SESSION_FILE).find("remembered-nmdc") != std::string::npos);
			printf("  ok a supplied password is written to the session file\n");
		}
		else failures++;
	}

	/*
	 * A password the hub refused is cleared, and the clearing reaches the file
	 * the same way the password did. A hand-edited one that turns out to be
	 * wrong does not survive being told so.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			std::string line;
			s.say("$GetPass");
			CHECK(s.hub.expect("$MyPass", line));

			s.session->sendPassword("wrong-one");
			CHECK(readFile(SESSION_FILE).find("wrong-one") != std::string::npos);

			s.say("$BadPass");
			CHECK(readFile(SESSION_FILE).find("wrong-one") == std::string::npos);
			printf("  ok a refused password is cleared from the session file\n");
		}
		else failures++;
	}

	/* A password nobody asked for is not sent. */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.session->sendPassword("unsolicited");
			pump([] { return false; }, 100);

			s.hub.drain();
			CHECK(s.hub.received.find("$MyPass") == std::string::npos);
			printf("  ok an unsolicited password is not sent\n");
		}
		else failures++;
	}

	/* A password the hub refused is forgotten, reported, and can be replaced. */
	{
		Session s;
		if (s.ready && s.login())
		{
			std::string line;

			s.say("$GetPass");
			CHECK(s.hub.expect("$MyPass", line));   /* the empty one */

			s.session->sendPassword("wrong");
			CHECK(s.hub.expect("$MyPass ", line));
			CHECK(line == "$MyPass wrong");

			s.listener.authRequested = false;
			s.say("$BadPass");
			CHECK(s.listener.authRequested);

			/* Still in the password state, so a second attempt goes through. */
			s.session->sendPassword("right");
			CHECK(s.hub.expect("$MyPass ", line));
			CHECK(line == "$MyPass right");
			printf("  ok a refused password is reported and can be replaced\n");
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

	/* ---- lines a hub sent, at the length the hub chose to send them ---- */

	/*
	 * $MyINFO read its nickname from a fixed offset without consulting the
	 * length at all, so a line this short was made from whatever followed it,
	 * and the mode flag and the '$' after the description were read from beyond
	 * the end of the line as well.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);

			CHECK(!h.call("$MyINFO", "$MyINFO"));
			CHECK(!h.call("$MyINFO", "$MyINFO "));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL"));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL "));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL alice"));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL alice x$"));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL alice x$ "));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL alice x$ $"));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL alice x$ $DSL"));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL alice x$ $DSL$m@e"));
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL alice x$ $DSL$m@e$1024"));

			/* A mode field longer than the one byte the syntax allows for it. */
			CHECK(!h.call("$MyINFO", "$MyINFO $ALL alice x$AP$DSL$m@e$1024$"));

			QuickDC::UserManager* users = s.session->getUserManager();
			CHECK(users && users->getUser("alice") == 0);
			printf("  ok a $MyINFO that stops early is refused\n");
		}
		else failures++;
	}

	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);
			QuickDC::UserManager* users = s.session->getUserManager();

			CHECK(h.call("$MyINFO",
				"$MyINFO $ALL alice a description$ $DSL\x01$mail@example$1024$"));

			const QuickDC::User* alice = users ? users->getUser("alice") : 0;
			CHECK(alice != 0);
			CHECK(alice && alice->getDescription()
				&& std::string(alice->getDescription()) == "a description");
			CHECK(alice && alice->getEmail()
				&& std::string(alice->getEmail()) == "mail@example");
			CHECK(alice && alice->getSharedBytes() == 1024);

			/* The tag is the last "<...>" of the description, and not part of it. */
			CHECK(h.call("$MyINFO",
				"$MyINFO $ALL bob shared<++ V:0.1,M:A,H:1/0/0,S:2>$ $DSL\x01$b@e$2048$"));

			const QuickDC::User* bob = users ? users->getUser("bob") : 0;
			CHECK(bob && bob->getDescription()
				&& std::string(bob->getDescription()) == "shared");
			CHECK(bob && bob->getNumSlots() == 2);
			CHECK(bob && bob->getUserAgent()
				&& std::string(bob->getUserAgent()) == "DC++ 0.1");
			printf("  ok a whole $MyINFO is read as it was\n");
		}
		else failures++;
	}

	/*
	 * $ConnectToMe with nothing in front of the address: the nick was taken from
	 * a fixed offset to a separator that was never found, so its length was
	 * measured from address zero.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);
			s.say("$Hello alice");

			CHECK(!h.call("$ConnectToMe", "$ConnectToMe 1.2.3.4:411"));
			CHECK(!h.call("$ConnectToMe", "$ConnectToMe"));
			CHECK(!h.call("$ConnectToMe", "$ConnectToMe "));
			CHECK(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4"));
			CHECK(!h.call("$ConnectToMe", "$ConnectToMe nick :411"));
			CHECK(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4:"));
			CHECK(s.listener.connectTo.empty());

			/* One nick is ours, and the hub is not saying who is calling. */
			CHECK(h.call("$ConnectToMe", "$ConnectToMe " + s.nick + " 127.0.0.1:411"));
			CHECK(s.listener.connectTo.size() == 1);
			CHECK(!s.listener.connectTo.empty()
				&& s.listener.connectTo[0] == "127.0.0.1:411");
			CHECK(s.listener.connectToKnown == 0);

			/* Two, and the first of them is the peer that asked. */
			CHECK(h.call("$ConnectToMe", "$ConnectToMe alice " + s.nick + " 127.0.0.1:412"));
			CHECK(s.listener.connectTo.size() == 2);
			CHECK(s.listener.connectTo.size() == 2
				&& s.listener.connectTo[1] == "127.0.0.1:412");
			CHECK(s.listener.connectToKnown == 1);
			printf("  ok $ConnectToMe needs a nick as well as an address\n");
		}
		else failures++;
	}

	/*
	 * $Search stopping inside its pattern. The size, the type and the query were
	 * all read from fixed offsets past the end, and the query that came out of
	 * it was then matched against the share.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);

			CHECK(!h.call("$Search", "$Search 1.2.3.4:1 T?T?"));
			CHECK(!h.call("$Search", "$Search 1.2.3.4:1 T?T?0?"));
			CHECK(!h.call("$Search", "$Search 1.2.3.4:1 T?T"));
			CHECK(!h.call("$Search", "$Search 1.2.3.4:1"));
			CHECK(!h.call("$Search", "$Search"));

			/* Too short to be either an address or a "Hub:{nick}". */
			CHECK(!h.call("$Search", "$Search abc F?F?0?1?x"));

			/* A flag of more than the single character it is. */
			CHECK(!h.call("$Search", "$Search 1.2.3.4:1 TT?T?0?1?x"));

			CHECK(h.call("$Search", "$Search 1.2.3.4:1 F?T?0?1?word"));
			CHECK(h.call("$Search", "$Search 1.2.3.4:1 T?F?1024?1?two$terms"));
			CHECK(h.call("$Search",
				"$Search Hub:alice F?F?0?9?TTH:2ZROV7YW7RTOKFJDF62WDMM2XGGC55Q3VF7WODI"));
			printf("  ok a $Search stopping short of its query is refused\n");
		}
		else failures++;
	}

	/*
	 * $Supports of exactly ten bytes: the prefix was taken off by length
	 * without checking there was one, leaving nothing, and the last character
	 * of nothing was then looked at.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);

			CHECK(!h.call("$Supports", "$Supports"));
			CHECK(h.call("$Supports", "$Supports "));
			CHECK(h.call("$Supports", "$Supports NoGetINFO UserIP2 "));
			CHECK(h.call("$Supports", "$Supports OpPlus"));
			printf("  ok a $Supports naming nothing is not an empty string read\n");
		}
		else failures++;
	}

	/*
	 * $UserIP had no length check whatever, and took its prefix off with
	 * substr(), which throws rather than returning anything - and nothing on the
	 * way here catches.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);
			s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
			s.say("$MyINFO $ALL bob y$ $DSL\x01$m@e$200$");

			CHECK(!h.call("$UserIP", "$UserIP"));
			CHECK(!h.call("$UserIP", "$UserI"));
			CHECK(!h.call("$UserIP", ""));

			const int before = s.listener.userUpdates;
			CHECK(h.call("$UserIP", "$UserIP alice 10.0.0.1"));
			CHECK(s.listener.userUpdates == before + 1);

			CHECK(h.call("$UserIP", "$UserIP alice 10.0.0.1$$bob 10.0.0.2$$"));
			CHECK(s.listener.userUpdates == before + 3);
			printf("  ok $UserIP needs the prefix it takes off\n");
		}
		else failures++;
	}

	/*
	 * $To: where the " $" that ends the sender's nick is inside the " From: "
	 * that was just matched, so the nick was a field of negative length.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);
			s.say("$Hello alice");

			CHECK(!h.call("$To:", "$To: aaa From: $x"));
			CHECK(!h.call("$To:", "$To:"));
			CHECK(!h.call("$To:", "$To: me From: alice"));
			CHECK(!h.call("$To:", "$To: me From: "));
			CHECK(s.listener.privateChat.empty());

			CHECK(h.call("$To:", "$To: " + s.nick + " From: alice $hello there"));
			CHECK(s.listener.privateChat.size() == 1);
			CHECK(!s.listener.privateChat.empty()
				&& s.listener.privateChat[0] == "hello there");

			/* A bot that repeats the sender's nick in the message body. */
			CHECK(h.call("$To:", "$To: " + s.nick + " From: alice $<alice> hi"));
			CHECK(s.listener.privateChat.size() == 2);
			CHECK(s.listener.privateChat.size() == 2
				&& s.listener.privateChat[1] == "hi");
			printf("  ok a $To: with no message is refused\n");
		}
		else failures++;
	}

	/*
	 * $Lock with the "Pk=" immediately after the keyword: the lock ran from
	 * after the prefix to before it, a negative length. $RevConnectToMe naming
	 * one nick where the syntax has two does the same.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);
			s.say("$Hello alice");

			CHECK(!h.call("$Lock", "$Lock Pk=abcdef"));
			CHECK(!h.call("$Lock", "$Lock"));
			CHECK(!h.call("$Lock", "$Lock abc"));
			CHECK(!h.call("$Lock", "$Lock abc Pk="));
			CHECK(h.call("$Lock", "$Lock " HUB_LOCK " Pk=TESTHUB"));

			CHECK(!h.call("$RevConnectToMe", "$RevConnectToMe alice"));
			CHECK(!h.call("$RevConnectToMe", "$RevConnectToMe"));
			CHECK(!h.call("$RevConnectToMe", "$RevConnectToMe  alice"));

			const int before = s.listener.passiveConnect;
			CHECK(h.call("$RevConnectToMe", "$RevConnectToMe alice " + s.nick));
			CHECK(s.listener.passiveConnect == before + 1);
			printf("  ok a $Lock and a $RevConnectToMe with a field missing are refused\n");
		}
		else failures++;
	}

	/* The rest of what a hub can say, with and without its arguments. */
	{
		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);
			QuickDC::UserManager* users = s.session->getUserManager();

			CHECK(!h.call("$Hello", "$Hello"));
			CHECK(!h.call("$Hello", "$Hello "));
			CHECK(h.call("$Hello", "$Hello carol"));
			CHECK(users && users->getUser("carol") != 0);

			CHECK(!h.call("$HubName", "$HubName"));
			CHECK(!h.call("$HubName", "$HubName "));
			CHECK(h.call("$HubName", "$HubName Some Hub"));
			CHECK(s.listener.hubName == "Some Hub");

			CHECK(!h.call("$Quit", "$Quit"));
			CHECK(!h.call("$Quit", "$Quit "));
			CHECK(h.call("$Quit", "$Quit carol"));
			CHECK(users && users->getUser("carol") == 0);

			CHECK(!h.call("$ForceMove", "$ForceMove"));
			CHECK(!h.call("$ForceMove", "$ForceMove "));
			CHECK(h.call("$ForceMove", "$ForceMove otherhub:412"));
			CHECK(s.listener.redirects.size() == 1);
			CHECK(!s.listener.redirects.empty()
				&& s.listener.redirects[0] == "otherhub:412");

			/* No port named, and 411 is the default in DC. */
			CHECK(h.call("$ForceMove", "$ForceMove otherhub"));
			CHECK(s.listener.redirects.size() == 2);
			CHECK(s.listener.redirects.size() == 2
				&& s.listener.redirects[1] == "otherhub:411");

			/* A list of nicks is nothing to refuse over, so these say so. */
			CHECK(h.call("$NickList", "$NickList"));
			CHECK(h.call("$NickList", "$NickList a$$b$$"));
			CHECK(h.call("$OpList", "$OpList"));
			CHECK(h.call("$OpList", "$OpList alice$$"));

			CHECK(!h.call("PublicChat", "<"));
			CHECK(!h.call("PublicChat", "<alice>"));
			CHECK(!h.call("PublicChat", "<> hello"));
			CHECK(h.call("PublicChat", "<alice> hello"));

			CHECK(!h.call("ActionChat", "*"));
			CHECK(!h.call("ActionChat", "* alice"));
			CHECK(h.call("ActionChat", "* alice waves"));
			printf("  ok every other command needs the arguments it reads\n");
		}
		else failures++;
	}

	/*
	 * Every prefix of a well formed line of each kind, which is the general
	 * case of all of the above: a handler that indexes a fixed offset reads past
	 * the end of the line for some length of every one of these.
	 */
	{
		static const struct { const char* name; const char* line; } lines[] = {
			{ "$Hello",          "$Hello alice" },
			{ "$HubName",        "$HubName A Hub" },
			{ "$Lock",           "$Lock " HUB_LOCK " Pk=TESTHUB" },
			{ "$MyINFO",         "$MyINFO $ALL alice a desc<++ V:0.1,M:A,H:1/0/0,S:2>$ "
			                     "$DSL\x01$m@e$1024$" },
			{ "$NickList",       "$NickList alice$$bob$$" },
			{ "$OpList",         "$OpList alice$$" },
			{ "$Quit",           "$Quit alice" },
			{ "$To:",            "$To: me From: alice $<alice> hello" },
			{ "$Search",         "$Search 1.2.3.4:412 F?T?0?9?"
			                     "TTH:2ZROV7YW7RTOKFJDF62WDMM2XGGC55Q3VF7WODI" },
			{ "$SR",             "$SR alice path\\file\x05" "1024 2/4\x05" "A Hub (1.2.3.4:411)" },
			{ "$Supports",       "$Supports NoGetINFO NoHello UserIP2" },
			{ "$UserIP",         "$UserIP alice 10.0.0.1$$bob 10.0.0.2$$" },
			{ "$UserCommand",    "$UserCommand 1 3 Name$Command" },
			{ "$ForceMove",      "$ForceMove otherhub:412" },
			{ "$ConnectToMe",    "$ConnectToMe alice me 127.0.0.1:412" },
			{ "$RevConnectToMe", "$RevConnectToMe alice me" },
			{ "$GetPass",        "$GetPass" },
			{ "$HubIsFull",      "$HubIsFull" },
			{ "$LogedIn",        "$LogedIn" },
			/*
			 * These two take no arguments, so there is no length for them to
			 * disagree with - but they are in the sweep because it repeats each
			 * line, and a refusal that released the nickname left every later
			 * reader of it, the destructor included, with a null pointer.
			 */
			{ "$ValidateDenide", "$ValidateDenide" },
			{ "$BadPass",        "$BadPass" },
			{ "PublicChat",      "<alice> hello there" },
			{ "ActionChat",      "* alice waves" },
		};

		Session s;
		if (s.ready && s.login())
		{
			Handlers h(s);
			s.say("$Hello alice");

			for (size_t n = 0; n < sizeof(lines)/sizeof(lines[0]); n++)
				for (size_t len = 0; len <= strlen(lines[n].line); len++)
					h.call(lines[n].name, std::string(lines[n].line, len));

			printf("  ok no handler reads past the length it was given\n");
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
	const std::string ses = SESSION_FILE;

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
