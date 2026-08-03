/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * A real ADC::HubSession talking to a hub that only exists for the test.
 *
 * The hub is a listening socket on a loopback port the kernel picks, and the
 * session connects to it the way it connects to any other: its own socket, its
 * own command proxy, the real event loop. Nothing here fakes the protocol
 * layer, so what is asserted is what would go over the wire.
 *
 * The one thing that is not real is the core. The full one creates
 * ~/.quickdc, reads and writes the user's configuration, scans the shares and
 * opens a listening socket; the session needs none of that, so this uses the
 * constructor that builds only the four pieces it does need, pointed at
 * scratch files.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/hub.h"
#include "network/adc/adccommon.h"
#include "network/adc/client/adchubsession.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "config/preferences.h"

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
static const char* SESSION_FILE = "/tmp/quickdc_hubtest_session.conf";

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

/* ------------------------------------------------------------------------- */

/* Runs the event loop until the condition holds, or gives up. */
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
		std::vector<bool> hubMarkdown;
		std::vector<std::string> joined;
		std::vector<std::string> left;
		std::vector<std::string> errors;
		std::string hubName;
		int netConnected = 0;
		int loggedIn = 0;
		bool authRequested = false;

		void EventNetStatus(enum StatusNetwork state) override
		{ if (state == NetConnected) netConnected++; }

		void EventHubStatus(enum StatusHub state) override
		{ if (state == HubLoggedIn) loggedIn++; }

		void EventSystemError(const char* msg) override
		{ errors.push_back(msg ? msg : ""); }

		void EventChat(const QuickDC::User*, const char* message, bool, bool) override
		{ chat.push_back(message ? message : ""); }

		void EventPrivateChat(const QuickDC::User*, const QuickDC::User*,
			const char* message, const char*, bool, bool) override
		{ privateChat.push_back(message ? message : ""); }

		void EventHubMessage(const char*, const char* message, bool,
			bool richText) override
		{
			hubMessages.push_back(message ? message : "");
			hubMarkdown.push_back(richText);
		}

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

/*
 * The other end of the connection: a listening socket, and whatever the client
 * sent, kept as a stream so it can be read a line at a time.
 */
class FakeHub :
	public Samurai::IO::Net::ServerSocketEventHandler,
	public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> peer;
		std::string received;
		bool disconnected = false;

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			return server && server->getFD() != -1 && server->listen();
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }

		/* Waits for the client's connection to be accepted. */
		bool accepted() { return pump([&] { return peer != 0; }); }

		void send(const std::string& line)
		{
			if (!peer) return;
			const std::string out = line + "\n";
			peer->write(out.data(), out.size());
		}

		/* Waits for a whole line and takes it out of the stream. */
		bool nextLine(std::string& out)
		{
			const bool got = pump([&] {
				drain();
				return received.find('\n') != std::string::npos;
			});

			if (!got) return false;

			const size_t at = received.find('\n');
			out = received.substr(0, at);
			received.erase(0, at + 1);
			return true;
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
		void EventDisconnected(const Samurai::IO::Net::Socket*) override { disconnected = true; }
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { }
		void EventCanWrite(const Samurai::IO::Net::Socket*) override { }
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override { }
};

/*
 * A hub, a session connected to it, and the login exchange completed - which
 * is where every case below starts.
 */
class Session
{
	public:
		FakeHub hub;
		Listener listener;
		std::unique_ptr<Samurai::IO::Net::URL> url;
		std::unique_ptr<QuickDC::Hub> ui;
		ADC::HubSession* session = 0;
		bool ready = false;

		Session()
		{
			if (!hub.listen()) return;

			const std::string address =
				"adc://127.0.0.1:" + std::to_string(hub.port());
			url = std::make_unique<Samurai::IO::Net::URL>(address);

			ui = std::make_unique<QuickDC::Hub>(&listener, url.get());
			session = new ADC::HubSession(ui.get(), &listener, url.get(), false);

			session->connect();
			if (!hub.accepted()) return;

			ready = true;
		}

		~Session()
		{
			delete session;
			/* The hub manager holds it until then. */
			ui.reset();
		}

		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		/* Everything the hub says arrives through here. */
		void say(const std::string& line)
		{
			hub.send(line);
			/* Let the session read and act on it. */
			pump([] { return false; }, 60);
		}

		/* The support exchange and a SID, which is what "logged in" means. */
		bool login(const char* sid = "AAAA")
		{
			std::string line;
			if (!hub.nextLine(line)) return false;
			if (line.compare(0, 4, "HSUP") != 0) return false;

			say(std::string("ISUP ADBASE ADTIGR"));
			say(std::string("ISID ") + sid);
			say("IINF NITestHub DEA test hub VE1.0");

			/*
			 * The client answers a SID with its own INF, and a hub echoes that
			 * back to everyone - including the client, which is how it learns
			 * it is on and gets an entry for itself.
			 */
			if (!hub.nextLine(line) || line.compare(0, 4, "BINF") != 0) return false;
			say(line);
			return true;
		}
};

/* ------------------------------------------------------------------------- */

static void run()
{
	/* ---- connecting ---- */

	{
		Session s;
		CHECK(s.ready);
		CHECK(s.listener.netConnected == 1);
		printf("  ok the session connects to the hub\n");
	}

	/* The first thing an ADC client says is what it supports. */
	{
		Session s;
		if (s.ready)
		{
			std::string line;
			CHECK(s.hub.nextLine(line));
			CHECK(line.compare(0, 4, "HSUP") == 0);
			CHECK(line.find("ADBASE") != std::string::npos);

			/*
			 * Named one by one rather than taken as a whole line, so that a flag
			 * going missing is a failure here and not something noticed on a hub.
			 * RTF0 is the one worth naming twice over: a hub only carries the RT
			 * flag on a message if both ends offered it, so a client that stops
			 * asking simply stops sending markdown, with nothing to show for it.
			 */
			CHECK(line.find("AD" ADC_HASH_TIGER) != std::string::npos);
			CHECK(line.find("AD" ADC_EXT_ADCS) != std::string::npos);
			CHECK(line.find("AD" ADC_EXT_PFSR) != std::string::npos);
			CHECK(line.find("AD" ADC_EXT_RICHTEXT) != std::string::npos);

			printf("  ok it opens with HSUP: %s\n", line.c_str());
		}
		else failures++;
	}

	/* A SID is answered with the client's own INF, carrying the nick. */
	{
		Session s;
		if (s.ready)
		{
			std::string line;
			CHECK(s.hub.nextLine(line));

			s.say("ISUP ADBASE ADTIGR");
			s.say("ISID AAAA");

			CHECK(s.hub.nextLine(line));
			CHECK(line.compare(0, 4, "BINF") == 0);
			/* Its own SID, and an ID and PID of its own making. */
			CHECK(line.find("AAAA") != std::string::npos);
			CHECK(line.find("NI") != std::string::npos);
			printf("  ok a SID is answered with BINF: %s\n", line.c_str());
		}
		else failures++;
	}

	/* ---- who is on the hub ---- */

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("BINF BBBB NIalice DEsomeone SS1024 SL3");

			QuickDC::UserManager* users = s.session->getUserManager();
			CHECK(users != 0);

			const QuickDC::User* alice = users ? users->getUser("BBBB") : 0;
			CHECK(alice != 0);
			CHECK(alice && alice->getNick() && !strcmp(alice->getNick(), "alice"));
			CHECK(alice && alice->getSharedBytes() == 1024);
			printf("  ok another user's INF puts them on the hub\n");
		}
		else failures++;
	}

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("BINF BBBB NIalice SS100");
			s.say("BINF CCCC NIbob SS200");

			QuickDC::UserManager* users = s.session->getUserManager();
			CHECK(users && users->getUser("BBBB") != 0);
			CHECK(users && users->getUser("CCCC") != 0);
			CHECK(users && users->countShared() == 300);

			s.say("IQUI BBBB");
			CHECK(users && users->getUser("BBBB") == 0);
			CHECK(users && users->getUser("CCCC") != 0);
			CHECK(users && users->countShared() == 200);
			printf("  ok a QUI takes the user and their share away\n");
		}
		else failures++;
	}

	/* An update to a user already there is not a second user. */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("BINF BBBB NIalice SS100");

			QuickDC::UserManager* users = s.session->getUserManager();
			const size_t before = users ? users->countUsers() : 0;

			s.say("BINF BBBB SS500");

			CHECK(users && users->countUsers() == before);
			const QuickDC::User* alice = users ? users->getUser("BBBB") : 0;
			CHECK(alice && alice->getSharedBytes() == 500);
			CHECK(alice && alice->getNick() && !strcmp(alice->getNick(), "alice"));
			printf("  ok an INF update does not duplicate the user\n");
		}
		else failures++;
	}

	/* ---- messages ---- */

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("BINF BBBB NIalice");
			s.say("BMSG BBBB hello\\severyone");

			CHECK(s.listener.chat.size() == 1);
			CHECK(!s.listener.chat.empty() && s.listener.chat[0] == "hello everyone");
			printf("  ok a chat message is unescaped and reported\n");
		}
		else failures++;
	}

	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("BINF BBBB NIalice");
			s.say("EMSG BBBB AAAA a\\sprivate\\sword PMBBBB");

			CHECK(s.listener.privateChat.size() == 1);
			CHECK(!s.listener.privateChat.empty()
				&& s.listener.privateChat[0] == "a private word");
			printf("  ok a private message is reported separately\n");
		}
		else failures++;
	}

	/*
	 * A hub speaks for itself with IMSG, which carries no SID, so the flags on it have
	 * to survive a path that has no user to attach them to.
	 */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("IMSG **welcome** RT1");
			s.say("IMSG plain\\senough");

			CHECK(s.listener.hubMessages.size() == 2);
			CHECK(s.listener.hubMarkdown.size() == 2);

			if (s.listener.hubMessages.size() == 2 && s.listener.hubMarkdown.size() == 2)
			{
				CHECK(s.listener.hubMessages[0] == "**welcome**");
				CHECK(s.listener.hubMarkdown[0]);
				CHECK(s.listener.hubMessages[1] == "plain enough");
				CHECK(!s.listener.hubMarkdown[1]);
			}

			printf("  ok the hub's own message keeps its RT flag\n");
		}
		else failures++;
	}

	/* The hub's own name reaches the interface. */
	{
		Session s;
		if (s.ready)
		{
			std::string line;
			s.hub.nextLine(line);
			s.say("ISUP ADBASE");
			s.say("ISID AAAA");
			s.say("IINF NIMy\\sHub DEwelcome");

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
			/* A message about a user who is not here. */
			s.say("BMSG ZZZZ from\\snobody");

			/* A quit for someone who never joined. */
			s.say("IQUI ZZZZ");

			/* Commands with nothing after the fourcc. */
			s.say("BINF");
			s.say("BMSG");
			s.say("IQUI");
			s.say("ISID");

			/* Not ADC at all. */
			s.say("this is not a command");
			s.say("");
			s.say("XXXX YYYY");

			QuickDC::UserManager* users = s.session->getUserManager();
			CHECK(users != 0);
			printf("  ok malformed and unknown commands are survived\n");
		}
		else failures++;
	}

	/*
	 * A hub asking for a password gets an answer derived from what was
	 * configured. Nothing here configures one, so the interface is told rather
	 * than the login failing with nothing reported.
	 */
	{
		Session s;
		if (s.ready)
		{
			/*
			 * GPA belongs to the identify phase: after the SID and the client's
			 * own INF, before the hub accepts it. A completed login is too late
			 * and the session says so rather than answering.
			 *
			 * Nothing here configures a password, so the request is put to the
			 * interface - and answered anyway, with the token hashed against
			 * the empty one. The hub refuses that, which is a faster and
			 * clearer end than waiting for an interface that may never answer.
			 */
			std::string line;
			CHECK(s.hub.nextLine(line));
			s.say("ISUP ADBASE");
			s.say("ISID AAAA");
			CHECK(s.hub.nextLine(line));
			CHECK(line.compare(0, 4, "BINF") == 0);

			s.say("IGPA SOMETOKEN");
			CHECK(s.listener.authRequested);

			CHECK(s.hub.nextLine(line));
			CHECK(line.compare(0, 4, "HPAS") == 0);
			printf("  ok a password request with none configured is reported and answered\n");
		}
		else failures++;
	}

	/* And answering it sends the token hashed with the password. */
	{
		Session s;
		if (s.ready)
		{
			std::string line;
			CHECK(s.hub.nextLine(line));
			s.say("ISUP ADBASE");
			s.say("ISID AAAA");
			CHECK(s.hub.nextLine(line));

			s.say("IGPA ABCDEFGHIJKLMNOP");
			CHECK(s.listener.authRequested);

			/* The empty answer went first; this is the second attempt. */
			CHECK(s.hub.nextLine(line));
			s.session->sendPassword("hunter2");

			CHECK(s.hub.nextLine(line));
			CHECK(line.compare(0, 4, "HPAS") == 0);
			/* A base32 tiger digest, not the password itself. */
			CHECK(line.size() > 5);
			CHECK(line.find("hunter2") == std::string::npos);
			printf("  ok answering the request sends a digest: %s\n", line.c_str());
		}
		else failures++;
	}

	/* The same password and token produce the same answer. */
	{
		std::string first;
		std::string second;

		for (int n = 0; n < 2; n++)
		{
			Session s;
			if (!s.ready) { failures++; break; }

			std::string line;
			s.hub.nextLine(line);
			s.say("ISUP ADBASE");
			s.say("ISID AAAA");
			s.hub.nextLine(line);

			s.say("IGPA ABCDEFGHIJKLMNOP");
			if (!s.hub.nextLine(line)) { failures++; break; }

			s.session->sendPassword("hunter2");
			if (!s.hub.nextLine(line)) { failures++; break; }
			(n == 0 ? first : second) = line;
		}

		CHECK(!first.empty() && first == second);
		printf("  ok the answer is derived from the password and the token\n");
	}

	/* And it is remembered, so the next run does not have to ask again. */
	{
		Session s;
		if (s.ready)
		{
			std::string line;
			CHECK(s.hub.nextLine(line));
			s.say("ISUP ADBASE");
			s.say("ISID AAAA");
			CHECK(s.hub.nextLine(line));

			s.say("IGPA ABCDEFGHIJKLMNOP");
			CHECK(s.hub.nextLine(line));

			s.session->sendPassword("remembered-adc");

			/* On disk before the session goes anywhere. */
			CHECK(readFile(SESSION_FILE).find("remembered-adc") != std::string::npos);
			printf("  ok a supplied password is written to the session file\n");
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
			CHECK(s.hub.received.find("HPAS") == std::string::npos);
			printf("  ok an unsolicited password is not sent\n");
		}
		else failures++;
	}

	/* A status message from the hub. */
	{
		Session s;
		if (s.ready && s.login())
		{
			s.say("ISTA 000 Welcome\\saboard");
			printf("  ok a status message is survived\n");
		}
		else failures++;
	}

	/* The hub going away is noticed. */
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

/*
 * When a hub is worth trying again, and how soon.
 *
 * The policy is a function of the reason and of how many attempts have failed, so
 * it can be read here without waiting for any of it.
 */
static void test_retry_policy()
{
	using L = QuickDC::HubListener;

	/* Nothing has failed yet, so there is nothing to wait for. */
	CHECK(QuickDC::Hub::retryDelay(L::NetDisconnected, 0) == -1);

	/*
	 * Unreachable: once, soon. The first connection to a local address on macOS is
	 * spent asking whether this application may make it, and fails - the second is
	 * the one that works. A route that is really not there will not appear because
	 * we asked twice.
	 */
	CHECK(QuickDC::Hub::retryDelay(L::NetErrorHostUnreachable, 1)
		== QUICKDC_HUB_RETRY_UNREACHABLE);
	CHECK(QuickDC::Hub::retryDelay(L::NetErrorHostUnreachable, 2) == -1);
	CHECK(QuickDC::Hub::retryDelay(L::NetErrorHostUnreachable, 9) == -1);
	printf("  ok a host that could not be reached is retried once, soon\n");

	/*
	 * Everything else backs off, doubling, and every wait lands in the upper half
	 * of what it doubled to - so a hub that threw every client off at once does not
	 * get all of them back in the same instant.
	 */
	const L::StatusNetwork reasons[] = {
		L::NetDisconnected,
		L::NetErrorConnectionRefused,
		L::NetErrorTimeout,
		L::NetErrorIO
	};

	for (size_t r = 0; r < sizeof(reasons) / sizeof(reasons[0]); r++)
	{
		int previous = 0;

		for (size_t attempt = 1; attempt <= 12; attempt++)
		{
			int lowest = QUICKDC_HUB_RETRY_MAX;
			int highest = 0;

			/* Sampled, because each answer carries its own spread. */
			for (int n = 0; n < 200; n++)
			{
				const int wait = QuickDC::Hub::retryDelay(reasons[r], attempt);

				CHECK(wait >= 1);
				CHECK(wait <= QUICKDC_HUB_RETRY_MAX);

				if (wait < lowest) lowest = wait;
				if (wait > highest) highest = wait;
			}

			/* Never sooner than the attempt before it asked for. */
			CHECK(lowest >= previous / 2);
			previous = lowest;

			/* And spread, rather than the same answer every time. */
			if (attempt > 2) CHECK(highest > lowest);
		}

		/* Twelve doublings from five seconds is well past the ceiling. */
		CHECK(QuickDC::Hub::retryDelay(reasons[r], 12) > QUICKDC_HUB_RETRY_MAX / 2);
	}

	printf("  ok everything else backs off to a ceiling, spread either side\n");
}

int main()
{
	setbuf(stdout, 0);

	const std::string cfg = "/tmp/quickdc_hubtest_config.conf";
	const std::string ses = SESSION_FILE;

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	{
		/*
		 * One for the whole process, and scoped: Preferences writes itself out
		 * when it is destroyed, so removing the files has to wait until the
		 * core holding them is gone.
		 */
		QuickDC::Core core(cfg, ses);
		run();
		test_retry_policy();
	}

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
