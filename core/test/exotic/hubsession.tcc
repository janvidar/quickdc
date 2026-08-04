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
#include "network/hubmanager.h"
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
#include <samurai/timer.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>

#include <chrono>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_hubtest_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/* Where the core under test keeps its session data. */
static std::string session_file;

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

static const int TIMEOUT_MS = 5000;

/* ------------------------------------------------------------------------- */

/* Runs the event loop until the condition holds, or gives up. */
template<typename Fn>
static bool pump(Fn done, int timeout_ms = TIMEOUT_MS)
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();
	Samurai::TimerManager* timers = Samurai::TimerManager::getInstance();

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);

	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		monitor->wait(10);
		/* Driven here too, so a case waiting on a timer does not depend on
		   wait() having done it. */
		timers->process();
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

		/* Whether the port could be bound at all, which is what tells a machine
		   that cannot apart from a client that will not connect: the first is a
		   case that is skipped, the second one that fails. */
		bool listening = false;

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			listening = server && server->getFD() != -1 && server->listen();
			return listening;
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

namespace {

/*
 * The fixture. One core for the whole process, because it is a singleton,
 * expensive to build, and has to be pointed at scratch files rather than the
 * user's own configuration.
 */
std::string config_file;
QuickDC::Core* core = 0;

}

EXO_SETUP({
	config_file = scratch("config.conf");
	session_file = scratch("session.conf");

	core = new QuickDC::Core(config_file, session_file);
	return 1;
});

EXO_TEARDOWN({
	/*
	 * Preferences writes itself out when it is destroyed, so removing the files
	 * has to wait until the core holding them is gone.
	 */
	delete core;
	core = 0;

	Samurai::IO::File(config_file).remove();
	Samurai::IO::File(session_file).remove();
	return 1;
});

/* ------------------------------------------------------------------------- */
/* connecting                                                                */
/* ------------------------------------------------------------------------- */

EXO_TEST(hubsession_the_session_connects_to_the_hub,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");

	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.listener.netConnected == 1);
	return 1;
});

/* The first thing an ADC client says is what it supports. */
EXO_TEST(hubsession_it_opens_with_hsup,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));

	if (line.compare(0, 4, "HSUP") != 0)
	{
		const std::string why = "the client opened with something else: " + line;
		EXO_FAIL(why.c_str());
	}
	EXO_ASSERT(line.find("ADBASE") != std::string::npos);

	/*
	 * Named one by one rather than taken as a whole line, so that a flag
	 * going missing is a failure here and not something noticed on a hub.
	 * RTF0 is the one worth naming twice over: a hub only carries the RT
	 * flag on a message if both ends offered it, so a client that stops
	 * asking simply stops sending markdown, with nothing to show for it.
	 */
	EXO_ASSERT(line.find("AD" ADC_HASH_TIGER) != std::string::npos);
	EXO_ASSERT(line.find("AD" ADC_EXT_ADCS) != std::string::npos);
	EXO_ASSERT(line.find("AD" ADC_EXT_PFSR) != std::string::npos);
	EXO_ASSERT(line.find("AD" ADC_EXT_RICHTEXT) != std::string::npos);
	return 1;
});

/* A SID is answered with the client's own INF, carrying the nick. */
EXO_TEST(hubsession_a_sid_is_answered_with_binf,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("ISUP ADBASE ADTIGR");
	s.say("ISID AAAA");

	EXO_ASSERT(s.hub.nextLine(line));

	if (line.compare(0, 4, "BINF") != 0)
	{
		const std::string why = "the SID was answered with something else: " + line;
		EXO_FAIL(why.c_str());
	}

	/* Its own SID, and an ID and PID of its own making. */
	EXO_ASSERT(line.find("AAAA") != std::string::npos);
	EXO_ASSERT(line.find("NI") != std::string::npos);
	return 1;
});

/*
 * A status update re-sends INF, which is how a hub is told that something it
 * was told once has changed - the port a peer should use being the case that
 * matters, since forwarding it succeeds long after the hub was joined.
 */
EXO_TEST(hubsession_a_status_update_re_sends_inf,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string line;
	/*
	 * Drained directly rather than by calling nextLine() until it fails:
	 * that waits for a line to arrive, so the last call would sit out the
	 * whole timeout for nothing and make this case the slowest here.
	 */
	s.hub.drain();
	s.hub.received.clear();

	s.session->sendClientInfo();
	pump([] { return false; }, 60);

	EXO_ASSERT(s.hub.nextLine(line));

	if (line.compare(0, 4, "BINF") != 0)
	{
		const std::string why = "the update was not an INF: " + line;
		EXO_FAIL(why.c_str());
	}
	EXO_ASSERT(line.find("NI") != std::string::npos);
	return 1;
});

/*
 * Asking for an update schedules one rather than sending it. Many asks inside
 * the window are one update: a client reconnecting to several hubs changes the
 * hub counts on every attempt, and a hub treats a client that says so each
 * time as flooding.
 */
EXO_TEST(hubsession_twenty_update_requests_send_nothing_straight_away,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	QuickDC::HubManager* hubs = QuickDC::Core::getInstance()->hubs;
	s.hub.drain();
	s.hub.received.clear();

	for (int n = 0; n < 20; n++) hubs->requestClientInfoUpdate();

	/* One timer, and nothing on the wire yet. */
	EXO_ASSERT(hubs->isClientInfoUpdatePending());
	pump([] { return false; }, 120);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.empty());
	return 1;
});

/*
 * And the update does go out when the window closes.
 *
 * Driven through Hub::connect() rather than by building the session directly,
 * because the fan-out walks the hub manager and reads each hub's session -
 * which only connect() sets. The delay is shortened so this does not sit out
 * the real one.
 */
EXO_TEST(hubsession_three_requests_inside_the_window_become_one_update,
{
	FakeHub fake;
	Listener listener;
	if (!fake.listen()) EXO_SKIP("could not listen on a loopback port");

	const std::string address = "adc://127.0.0.1:" + std::to_string(fake.port());
	Samurai::IO::Net::URL url(address);
	QuickDC::Hub ui(&listener, &url);

	QuickDC::HubManager* hubs = QuickDC::Core::getInstance()->hubs;
	ui.connect();

	EXO_ASSERT(fake.accepted());

	std::string line;
	/* The support exchange, a SID, and the INF echoed back, which is what
	   being logged in means. */
	EXO_ASSERT(fake.nextLine(line));
	fake.send("ISUP ADBASE ADTIGR");
	pump([] { return false; }, 60);
	fake.send("ISID AAAA");
	pump([] { return false; }, 60);
	if (fake.nextLine(line) && line.compare(0, 4, "BINF") == 0)
	{
		fake.send(line);
		pump([] { return false; }, 60);
	}

	fake.drain();
	fake.received.clear();

	/*
	 * The manager is the process-wide one, so an earlier case may have left
	 * a request pending with the long window - which this one would be
	 * folded into, exactly as intended. Flushed first so what is measured
	 * here is this case's own requests.
	 */
	hubs->sendClientInfoToAllHubs();
	pump([] { return false; }, 60);
	fake.drain();
	fake.received.clear();

	hubs->setClientInfoUpdateDelay(std::chrono::milliseconds(50));
	hubs->requestClientInfoUpdate();
	hubs->requestClientInfoUpdate();
	hubs->requestClientInfoUpdate();

	const bool arrived = pump([&] {
		fake.drain();
		return fake.received.find("BINF") != std::string::npos;
	}, 2000);

	const bool pending = hubs->isClientInfoUpdatePending();

	/* Three requests, one INF. */
	size_t infs = 0;
	for (size_t at = fake.received.find("BINF"); at != std::string::npos;
	     at = fake.received.find("BINF", at + 1)) infs++;

	/* Put back before anything is asserted, since a failed assertion leaves. */
	hubs->setClientInfoUpdateDelay(
		std::chrono::seconds(QUICKDC_HUB_INFO_UPDATE_DELAY_SECONDS));

	EXO_ASSERT(arrived);
	EXO_ASSERT(!pending);
	EXO_ASSERT(infs == 1);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* who is on the hub                                                         */
/* ------------------------------------------------------------------------- */

EXO_TEST(hubsession_another_users_inf_puts_them_on_the_hub,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice DEsomeone SS1024 SL3");

	QuickDC::UserManager* users = s.session->getUserManager();
	EXO_ASSERT(users != 0);

	const QuickDC::User* alice = users ? users->getUser("BBBB") : 0;
	EXO_ASSERT(alice != 0);
	EXO_ASSERT(alice && alice->getNick() && !strcmp(alice->getNick(), "alice"));
	EXO_ASSERT(alice && alice->getSharedBytes() == 1024);
	return 1;
});

EXO_TEST(hubsession_a_qui_takes_the_user_and_their_share_away,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice SS100");
	s.say("BINF CCCC NIbob SS200");

	QuickDC::UserManager* users = s.session->getUserManager();
	EXO_ASSERT(users && users->getUser("BBBB") != 0);
	EXO_ASSERT(users && users->getUser("CCCC") != 0);
	EXO_ASSERT(users && users->countShared() == 300);

	s.say("IQUI BBBB");
	EXO_ASSERT(users && users->getUser("BBBB") == 0);
	EXO_ASSERT(users && users->getUser("CCCC") != 0);
	EXO_ASSERT(users && users->countShared() == 200);
	return 1;
});

/* An update to a user already there is not a second user. */
EXO_TEST(hubsession_an_inf_update_does_not_duplicate_the_user,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice SS100");

	QuickDC::UserManager* users = s.session->getUserManager();
	const size_t before = users ? users->countUsers() : 0;

	s.say("BINF BBBB SS500");

	EXO_ASSERT(users && users->countUsers() == before);
	const QuickDC::User* alice = users ? users->getUser("BBBB") : 0;
	EXO_ASSERT(alice && alice->getSharedBytes() == 500);
	EXO_ASSERT(alice && alice->getNick() && !strcmp(alice->getNick(), "alice"));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* messages                                                                  */
/* ------------------------------------------------------------------------- */

EXO_TEST(hubsession_a_chat_message_is_unescaped_and_reported,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice");
	s.say("BMSG BBBB hello\\severyone");

	EXO_ASSERT(s.listener.chat.size() == 1);
	EXO_ASSERT(!s.listener.chat.empty() && s.listener.chat[0] == "hello everyone");
	return 1;
});

EXO_TEST(hubsession_a_private_message_is_reported_separately,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice");
	s.say("EMSG BBBB AAAA a\\sprivate\\sword PMBBBB");

	EXO_ASSERT(s.listener.privateChat.size() == 1);
	EXO_ASSERT(!s.listener.privateChat.empty()
		&& s.listener.privateChat[0] == "a private word");
	return 1;
});

/*
 * A hub speaks for itself with IMSG, which carries no SID, so the flags on it have
 * to survive a path that has no user to attach them to.
 */
EXO_TEST(hubsession_the_hubs_own_message_keeps_its_rt_flag,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("IMSG **welcome** RT1");
	s.say("IMSG plain\\senough");

	EXO_ASSERT(s.listener.hubMessages.size() == 2);
	EXO_ASSERT(s.listener.hubMarkdown.size() == 2);

	if (s.listener.hubMessages.size() == 2 && s.listener.hubMarkdown.size() == 2)
	{
		EXO_ASSERT(s.listener.hubMessages[0] == "**welcome**");
		EXO_ASSERT(s.listener.hubMarkdown[0]);
		EXO_ASSERT(s.listener.hubMessages[1] == "plain enough");
		EXO_ASSERT(!s.listener.hubMarkdown[1]);
	}
	return 1;
});

/* The hub's own name reaches the interface. */
EXO_TEST(hubsession_the_hubs_name_reaches_the_interface,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	s.hub.nextLine(line);
	s.say("ISUP ADBASE");
	s.say("ISID AAAA");
	s.say("IINF NIMy\\sHub DEwelcome");

	EXO_ASSERT(s.listener.hubName == "My Hub");
	return 1;
});

/* ------------------------------------------------------------------------- */
/* what a hub can throw at a client                                          */
/* ------------------------------------------------------------------------- */

EXO_TEST(hubsession_malformed_and_unknown_commands_are_survived,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

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
	EXO_ASSERT(users != 0);
	return 1;
});

/*
 * A hub asking for a password gets an answer derived from what was
 * configured. Nothing here configures one, so the interface is told rather
 * than the login failing with nothing reported.
 */
EXO_TEST(hubsession_a_password_request_with_none_configured_is_reported_and_answered,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

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
	EXO_ASSERT(s.hub.nextLine(line));
	s.say("ISUP ADBASE");
	s.say("ISID AAAA");
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "BINF") == 0);

	s.say("IGPA SOMETOKEN");
	EXO_ASSERT(s.listener.authRequested);

	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HPAS") == 0);
	return 1;
});

/* And answering it sends the token hashed with the password. */
EXO_TEST(hubsession_answering_the_request_sends_a_digest,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	s.say("ISUP ADBASE");
	s.say("ISID AAAA");
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("IGPA ABCDEFGHIJKLMNOP");
	EXO_ASSERT(s.listener.authRequested);

	/* The empty answer went first; this is the second attempt. */
	EXO_ASSERT(s.hub.nextLine(line));
	s.session->sendPassword("hunter2");

	EXO_ASSERT(s.hub.nextLine(line));

	if (line.compare(0, 4, "HPAS") != 0)
	{
		const std::string why = "the answer was not an HPAS: " + line;
		EXO_FAIL(why.c_str());
	}

	/* A base32 tiger digest, not the password itself. */
	EXO_ASSERT(line.size() > 5);
	EXO_ASSERT(line.find("hunter2") == std::string::npos);
	return 1;
});

/* The same password and token produce the same answer. */
EXO_TEST(hubsession_the_answer_is_derived_from_the_password_and_the_token,
{
	std::string first;
	std::string second;

	for (int n = 0; n < 2; n++)
	{
		Session s;
		if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
		EXO_ASSERT(s.ready);

		std::string line;
		s.hub.nextLine(line);
		s.say("ISUP ADBASE");
		s.say("ISID AAAA");
		s.hub.nextLine(line);

		s.say("IGPA ABCDEFGHIJKLMNOP");
		EXO_ASSERT(s.hub.nextLine(line));

		s.session->sendPassword("hunter2");
		EXO_ASSERT(s.hub.nextLine(line));
		(n == 0 ? first : second) = line;
	}

	EXO_ASSERT(!first.empty() && first == second);
	return 1;
});

/* And it is remembered, so the next run does not have to ask again. */
EXO_TEST(hubsession_a_supplied_password_is_written_to_the_session_file,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	s.say("ISUP ADBASE");
	s.say("ISID AAAA");
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("IGPA ABCDEFGHIJKLMNOP");
	EXO_ASSERT(s.hub.nextLine(line));

	s.session->sendPassword("remembered-adc");

	/* On disk before the session goes anywhere. */
	EXO_ASSERT(readFile(session_file.c_str()).find("remembered-adc")
		!= std::string::npos);
	return 1;
});

/* A password nobody asked for is not sent. */
EXO_TEST(hubsession_an_unsolicited_password_is_not_sent,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.session->sendPassword("unsolicited");
	pump([] { return false; }, 100);

	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("HPAS") == std::string::npos);
	return 1;
});

/* A status message from the hub. */
EXO_TEST(hubsession_a_status_message_is_survived,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("ISTA 000 Welcome\\saboard");
	return 1;
});

/* The hub going away is noticed. */
EXO_TEST(hubsession_the_session_notices_the_hub_going_away,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(s.session->isLoggedIn());

	s.hub.peer.reset();
	EXO_ASSERT(pump([&] { return !s.session->isLoggedIn(); }));
	return 1;
});

/* Sessions come and go without leaving anything in the monitor. */
EXO_TEST(hubsession_repeated_sessions_leave_nothing_behind,
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();
	const size_t baseline = monitor->size();

	for (int n = 0; n < 3; n++)
	{
		Session s;
		if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
		EXO_ASSERT(s.ready);
		s.login();
	}

	pump([&] { return monitor->size() <= baseline; }, 2000);
	EXO_ASSERT(monitor->size() == baseline);
	return 1;
});

/* ------------------------------------------------------------------------- */
/*
 * When a hub is worth trying again, and how soon.
 *
 * The policy is a function of the reason and of how many attempts have failed, so
 * it can be read here without waiting for any of it.
 */
/* ------------------------------------------------------------------------- */

EXO_TEST(hubsession_a_host_that_could_not_be_reached_is_retried_once_soon,
{
	using L = QuickDC::HubListener;

	/* Nothing has failed yet, so there is nothing to wait for. */
	EXO_ASSERT(QuickDC::Hub::retryDelay(L::NetDisconnected, 0) == -1);

	/*
	 * Unreachable: once, soon. The first connection to a local address on macOS is
	 * spent asking whether this application may make it, and fails - the second is
	 * the one that works. A route that is really not there will not appear because
	 * we asked twice.
	 */
	EXO_ASSERT(QuickDC::Hub::retryDelay(L::NetErrorHostUnreachable, 1)
		== QUICKDC_HUB_RETRY_UNREACHABLE);
	EXO_ASSERT(QuickDC::Hub::retryDelay(L::NetErrorHostUnreachable, 2) == -1);
	EXO_ASSERT(QuickDC::Hub::retryDelay(L::NetErrorHostUnreachable, 9) == -1);
	return 1;
});

/*
 * Everything else backs off, doubling, and every wait lands in the upper half
 * of what it doubled to - so a hub that threw every client off at once does not
 * get all of them back in the same instant.
 */
EXO_TEST(hubsession_everything_else_backs_off_to_a_ceiling_spread_either_side,
{
	using L = QuickDC::HubListener;

	const L::StatusNetwork reasons[] = {
		L::NetDisconnected,
		L::NetErrorConnectionRefused,
		L::NetErrorTimeout,
		L::NetErrorIO
	};

	for (size_t r = 0; r < sizeof(reasons) / sizeof(reasons[0]); r++)
	{
		const std::string which = "reason " + std::to_string((int) r);
		int previous = 0;

		for (size_t attempt = 1; attempt <= 12; attempt++)
		{
			const std::string where =
				which + " attempt " + std::to_string((int) attempt);

			int lowest = QUICKDC_HUB_RETRY_MAX;
			int highest = 0;

			/* Sampled, because each answer carries its own spread. */
			for (int n = 0; n < 200; n++)
			{
				const int wait = QuickDC::Hub::retryDelay(reasons[r], attempt);

				if (wait < 1 || wait > QUICKDC_HUB_RETRY_MAX)
				{
					const std::string why = where + " asked to wait "
						+ std::to_string(wait) + " seconds";
					EXO_FAIL(why.c_str());
				}

				if (wait < lowest) lowest = wait;
				if (wait > highest) highest = wait;
			}

			/* Never sooner than the attempt before it asked for. */
			if (lowest < previous / 2)
			{
				const std::string why = where + " came back sooner than the one before"
					" it: " + std::to_string(lowest) + " against "
					+ std::to_string(previous);
				EXO_FAIL(why.c_str());
			}
			previous = lowest;

			/* And spread, rather than the same answer every time. */
			if (attempt > 2 && highest <= lowest)
			{
				const std::string why = where + " answered "
					+ std::to_string(lowest) + " every time";
				EXO_FAIL(why.c_str());
			}
		}

		/* Twelve doublings from five seconds is well past the ceiling. */
		if (QuickDC::Hub::retryDelay(reasons[r], 12) <= QUICKDC_HUB_RETRY_MAX / 2)
		{
			const std::string why = which + " never reaches the ceiling";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});
