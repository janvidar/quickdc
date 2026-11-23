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
 *
 * QuickDC::Core reads its configuration from disk and runs on samurai's
 * process-wide reactor, so it is a per-file fixture. Each case builds its own hub and its own session: every one
 * of them is a separate process under CTest, so nothing a case leaves behind is
 * there for the next one to read.
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

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

/* The fixture's core; built in EXO_SETUP below. */
static QuickDC::Core* core = 0;

/*
 * Where the core under test keeps its configuration and its session data.
 * Named after the process so that two of these running at once do not read and
 * remove each other's files.
 */
static std::string configFile;
static std::string sessionFile;


static std::string scratch(const char* leaf)
{
	return "/tmp/quickdc_dchubtest_" + std::to_string((int) getpid()) + "_" + leaf;
}

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
		std::string hubDescription;
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

		void EventChat(const QuickDC::User*, const char* message, bool, bool,
		               uint64_t) override
		{ chat.push_back(message ? message : ""); }

		void EventPrivateChat(const QuickDC::User*, const QuickDC::User*,
			const char* message, const char*, bool, bool, uint64_t) override
		{ privateChat.push_back(message ? message : ""); }

		void EventHubMessage(const char*, const char* message, bool, bool,
		                     uint64_t) override
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

		void EventHubDescription(const char* description) override
		{ hubDescription = description ? description : ""; }

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
		/* Read as soon as it is offered rather than only when a case asks for it:
		   the monitor is level triggered, so a socket left unread is reported on
		   every pass and the loop spins until something takes the bytes off it. */
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { drain(); }
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

		/*
		 * Told apart so that a host which cannot bind a loopback port is a case
		 * that could not run, where a hub that never saw the connection is one
		 * that did and went wrong.
		 */
		bool listened = false;
		bool ready = false;

		Session()
		{
			if (!hub.listen()) return;
			listened = true;

			const std::string address =
				"dchub://127.0.0.1:" + std::to_string(hub.port());
			url = std::make_unique<Samurai::IO::Net::URL>(address);

			ui = std::make_unique<QuickDC::Hub>(*core, &listener, url.get());
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

		/*
		 * The names a case asked for that the proxy has no handler for. A
		 * missing one cannot be reported through the return value, because
		 * false is what a handler that refused the line answers - so a case
		 * expecting a refusal would pass without the handler having been
		 * reached at all. Callers assert on everyHandlerFound() below.
		 */
		std::vector<std::string> missing;

		bool call(const char* name, const std::string& line)
		{
			const auto it = mapper.find(name);
			if (it == mapper.end())
			{
				if (std::find(missing.begin(), missing.end(), name) == missing.end())
					missing.push_back(name);
				return false;
			}

			const std::vector<char> exact(line.begin(), line.end());
			return it->second->invoke(exact.data(), exact.size());
		}
};

/* Names which handler was missing rather than only that one was. */
static bool everyHandlerFound(const Handlers& h)
{
	for (const std::string& name : h.missing)
		printf("    no handler for %s\n", name.c_str());
	return h.missing.empty();
}

/* ------------------------------------------------------------------------- */

EXO_SETUP({
	configFile = scratch("config.conf");
	sessionFile = scratch("session.conf");

	Samurai::IO::File(configFile).remove();
	Samurai::IO::File(sessionFile).remove();

	core = new QuickDC::Core(configFile, sessionFile);
	return 1;
});

EXO_TEARDOWN({
	/* Preferences writes itself out when destroyed, so the files can only be
	   removed once the core holding them is gone. */
	delete core;
	core = 0;

	Samurai::IO::File(configFile).remove();
	Samurai::IO::File(sessionFile).remove();
	return 1;
});

/* ------------------------------------------------------------------------- */

EXO_TEST(dchubsession_the_session_connects_to_the_hub,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");

	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.listener.netConnected == 1);
	return 1;
});

/* The hub speaks first, and the client answers the lock with a key. */
EXO_TEST(dchubsession_a_lock_is_answered_with_a_key,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);

	s.say("$Lock " HUB_LOCK " Pk=TESTHUB");

	std::string line;
	EXO_ASSERT(s.hub.expect("$Key ", line));
	EXO_ASSERT(line.size() > strlen("$Key "));
	return 1;
});

/* An extended-protocol lock also gets $Supports. */
EXO_TEST(dchubsession_an_extended_lock_is_answered_with_supports,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);

	s.say("$Lock " HUB_LOCK " Pk=TESTHUB");

	std::string line;
	EXO_ASSERT(s.hub.expect("$Supports ", line));
	return 1;
});

/* And then the nick it wants. */
EXO_TEST(dchubsession_it_asks_to_validate_a_nick,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);

	s.say("$Lock " HUB_LOCK " Pk=TESTHUB");

	std::string line;
	EXO_ASSERT(s.hub.expect("$ValidateNick ", line));
	EXO_ASSERT(line.size() > strlen("$ValidateNick "));
	return 1;
});

/*
 * A lock too short to derive a key from arrives before anything has been
 * verified, and the connection is dropped rather than answered.
 */
EXO_TEST(dchubsession_a_lock_too_short_to_answer_is_refused,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);

	s.say("$Lock ab Pk=x");
	pump([] { return false; }, 200);

	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("$Key") == std::string::npos);
	return 1;
});

/* $Hello for our own nick is what asks for our info. */
EXO_TEST(dchubsession_hello_is_answered_with_myinfo,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	return 1;
});

/*
 * A status update re-sends $MyINFO. This did nothing at all until port
 * forwarding needed it: the base class declares sendClientInfo() as the way to
 * tell a hub that something changed, ADC implemented it, and the NMDC override
 * was an empty body - so an NMDC hub kept whatever it was told at login for as
 * long as the session lasted.
 */
EXO_TEST(dchubsession_a_status_update_re_sends_myinfo,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string line;
	s.hub.drain();
	s.hub.received.clear();

	s.session->sendClientInfo();
	pump([] { return false; }, 60);

	EXO_ASSERT(s.hub.expect("$MyINFO ", line));
	return 1;
});

/* And nothing is sent before there is a hub that would understand it. */
EXO_TEST(dchubsession_a_status_update_before_login_sends_nothing,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);

	s.hub.drain();
	s.hub.received.clear();

	s.session->sendClientInfo();
	pump([] { return false; }, 60);

	s.hub.drain();
	EXO_ASSERT(s.hub.received.empty());
	return 1;
});

/* ---- who is on the hub ---- */

EXO_TEST(dchubsession_hello_for_someone_else_puts_them_on_the_hub,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$Hello alice");

	QuickDC::UserManager* users = s.session->getUserManager();
	EXO_ASSERT(users != 0);
	EXO_ASSERT(users && users->getUser("alice") != 0);
	return 1;
});

EXO_TEST(dchubsession_myinfo_carries_the_share_size,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$MyINFO $ALL alice a description$ $DSL\x01$mail@example$1024$");

	QuickDC::UserManager* users = s.session->getUserManager();
	const QuickDC::User* alice = users ? users->getUser("alice") : 0;

	EXO_ASSERT(alice != 0);
	EXO_ASSERT(alice && alice->getSharedBytes() == 1024);
	EXO_ASSERT(users && users->countShared() == 1024);
	return 1;
});

EXO_TEST(dchubsession_quit_takes_the_user_and_their_share_away,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
	s.say("$MyINFO $ALL bob y$ $DSL\x01$m@e$200$");

	QuickDC::UserManager* users = s.session->getUserManager();
	EXO_ASSERT(users && users->countShared() == 300);

	s.say("$Quit alice");
	EXO_ASSERT(users && users->getUser("alice") == 0);
	EXO_ASSERT(users && users->getUser("bob") != 0);
	EXO_ASSERT(users && users->countShared() == 200);
	return 1;
});

/*
 * The status byte on the end of the speed field, of which only the away bit is
 * read. 0x01 is set by everybody and means nothing; 0x02 is away.
 */
EXO_TEST(dchubsession_myinfo_reads_the_away_bit_off_the_speed_field,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
	s.say("$MyINFO $ALL bob y$ $DSL\x03$m@e$200$");

	QuickDC::UserManager* users = s.session->getUserManager();
	const QuickDC::User* alice = users ? users->getUser("alice") : 0;
	const QuickDC::User* bob = users ? users->getUser("bob") : 0;

	EXO_ASSERT(alice && !alice->isAway());
	EXO_ASSERT(bob && bob->isAway());

	/* And coming back is another $MyINFO with the bit cleared. */
	s.say("$MyINFO $ALL bob y$ $DSL\x01$m@e$200$");
	EXO_ASSERT(bob && !bob->isAway());

	/* A hub that sends no speed at all says nothing about being away either. */
	s.say("$MyINFO $ALL carol z$ $$m@e$300$");
	const QuickDC::User* carol = users ? users->getUser("carol") : 0;
	EXO_ASSERT(carol && !carol->isAway());
	return 1;
});

/*
 * $UserIP names where somebody is. One field carries either family, and the hub
 * may answer for one user or for the whole hub at once.
 */
EXO_TEST(dchubsession_userip_files_an_address_against_the_user,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
	s.say("$MyINFO $ALL bob y$ $DSL\x01$m@e$200$");

	QuickDC::UserManager* users = s.session->getUserManager();
	const QuickDC::User* alice = users ? users->getUser("alice") : 0;
	const QuickDC::User* bob = users ? users->getUser("bob") : 0;

	/* The list form, which is how a hub answers $UserIP2. */
	s.say("$UserIP alice 192.168.0.7$$bob 2001:db8::1$$");

	EXO_ASSERT(alice && !strcmp(alice->getAddressIPv4(), "192.168.0.7"));
	EXO_ASSERT(bob && !strcmp(bob->getAddressIPv6(), "2001:db8::1"));

	/* And the single form, for somebody who arrived afterwards. */
	s.say("$UserIP alice 10.0.0.9");
	EXO_ASSERT(alice && !strcmp(alice->getAddressIPv4(), "10.0.0.9"));

	/* A hub may send anything; only an address is kept. */
	s.say("$UserIP alice hostname.example.org");
	EXO_ASSERT(alice && !strcmp(alice->getAddressIPv4(), "10.0.0.9"));
	return 1;
});

/*
 * $OpList is the whole list of operators rather than one name, and the hub
 * resends it entire whenever it changes.
 */
EXO_TEST(dchubsession_oplist_marks_the_operators_and_only_those,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
	s.say("$MyINFO $ALL bob y$ $DSL\x01$m@e$200$");

	QuickDC::UserManager* users = s.session->getUserManager();
	const QuickDC::User* alice = users ? users->getUser("alice") : 0;
	const QuickDC::User* bob = users ? users->getUser("bob") : 0;

	s.say("$OpList alice$$");
	EXO_ASSERT(alice && alice->isOp());
	EXO_ASSERT(bob && !bob->isOp());

	/* A resend without a name is how the hub says they have lost it. */
	s.say("$OpList bob$$");
	EXO_ASSERT(alice && !alice->isOp());
	EXO_ASSERT(bob && bob->isOp());

	/*
	 * A name can be in the list before their info is: the client asks after
	 * each nick separately and the answers arrive when they arrive.
	 */
	s.say("$OpList bob$$carol$$");
	s.say("$MyINFO $ALL carol z$ $DSL\x01$m@e$300$");

	const QuickDC::User* carol = users ? users->getUser("carol") : 0;
	EXO_ASSERT(carol && carol->isOp());
	return 1;
});

/* A second $MyINFO for the same nick is an update, not a new user. */
EXO_TEST(dchubsession_a_second_myinfo_updates_rather_than_duplicates,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");

	QuickDC::UserManager* users = s.session->getUserManager();
	const size_t before = users ? users->countUsers() : 0;

	s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$500$");

	EXO_ASSERT(users && users->countUsers() == before);
	const QuickDC::User* alice = users ? users->getUser("alice") : 0;
	EXO_ASSERT(alice && alice->getSharedBytes() == 500);
	return 1;
});

/*
 * $NickList is only a list of names. Unless the hub said NoGetINFO, the
 * client asks after each of them, and they arrive as $MyINFO.
 */
EXO_TEST(dchubsession_nicklist_is_followed_up_with_getinfo_for_each_nick,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$NickList alice$$bob$$carol$$");

	std::string line;
	EXO_ASSERT(s.hub.expect("$GetINFO alice ", line));
	EXO_ASSERT(s.hub.expect("$GetINFO bob ", line));
	EXO_ASSERT(s.hub.expect("$GetINFO carol ", line));

	/* Nobody is on the hub until the replies arrive. */
	QuickDC::UserManager* users = s.session->getUserManager();
	EXO_ASSERT(users && users->getUser("alice") == 0);

	s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
	EXO_ASSERT(users && users->getUser("alice") != 0);
	return 1;
});

/* ---- messages ---- */

EXO_TEST(dchubsession_public_chat_is_attributed_and_reported,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$Hello alice");
	s.say("<alice> hello everyone");

	EXO_ASSERT(s.listener.chat.size() == 1);
	EXO_ASSERT(!s.listener.chat.empty() && s.listener.chat[0] == "hello everyone");
	return 1;
});

EXO_TEST(dchubsession_a_private_message_is_reported_separately,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$Hello alice");
	s.say("$To: " + s.nick + " From: alice $<alice> a private word");

	EXO_ASSERT(s.listener.privateChat.size() == 1);
	EXO_ASSERT(!s.listener.privateChat.empty()
		&& s.listener.privateChat[0].find("a private word") != std::string::npos);
	return 1;
});

EXO_TEST(dchubsession_the_hubs_name_reaches_the_interface,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);

	s.say("$Lock " HUB_LOCK " Pk=TESTHUB");
	s.say("$HubName My Hub");

	EXO_ASSERT(s.listener.hubName == "My Hub");
	return 1;
});

/*
 * The topic, where a hub sends one. Not asked for in $Supports - see the parser -
 * so this is only the hubs that send it to everybody.
 */
EXO_TEST(dchubsession_a_hub_topic_is_reported_as_the_description,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);

	s.say("$Lock " HUB_LOCK " Pk=TESTHUB");
	s.say("$HubName My Hub");
	s.say("$HubTopic films, music and a place to argue about them");

	EXO_ASSERT(s.listener.hubName == "My Hub");
	EXO_ASSERT(s.listener.hubDescription
		== "films, music and a place to argue about them");

	/* Nothing after the keyword is not a topic of nothing. */
	s.say("$HubTopic");

	EXO_ASSERT(s.listener.hubDescription
		== "films, music and a place to argue about them");
	return 1;
});

/* ---- what a hub can throw at a client ---- */

EXO_TEST(dchubsession_malformed_and_unknown_commands_are_survived,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	/* Chat from, and a quit for, someone who is not here. */
	s.say("<nobody> talking");
	s.say("$Quit nobody");

	/* Commands with nothing after the keyword. */
	s.say("$Hello");
	s.say("$Quit");
	s.say("$MyINFO");
	s.say("$NickList");
	s.say("$HubName");
	s.say("$HubTopic");
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
	EXO_ASSERT(users != 0);
	return 1;
});

/*
 * Nothing here configures a password, so the request is put to the
 * interface - and answered anyway, with the empty one. The hub refuses
 * that, which is a faster and clearer end than waiting for an interface
 * that may never answer.
 */
EXO_TEST(dchubsession_a_password_request_with_none_configured_is_reported_and_answered,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$GetPass");
	EXO_ASSERT(s.listener.authRequested);

	std::string line;
	EXO_ASSERT(s.hub.expect("$MyPass", line));
	return 1;
});

/* A password supplied while the request is still open is sent. */
EXO_TEST(dchubsession_a_password_supplied_afterwards_is_sent,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("$GetPass");

	std::string line;
	EXO_ASSERT(s.hub.expect("$MyPass", line));

	s.session->sendPassword("hunter2");

	EXO_ASSERT(s.hub.expect("$MyPass ", line));
	EXO_ASSERT(line == "$MyPass hunter2");
	return 1;
});

/*
 * A password the user supplied is remembered, so the next run does not ask
 * again. Written when it is supplied rather than when the session ends, so
 * that a crash does not lose it.
 */
EXO_TEST(dchubsession_a_supplied_password_is_written_to_the_session_file,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string line;
	s.say("$GetPass");
	EXO_ASSERT(s.hub.expect("$MyPass", line));

	s.session->sendPassword("remembered-nmdc");

	/* On disk before the session goes anywhere. */
	EXO_ASSERT(readFile(sessionFile.c_str()).find("remembered-nmdc") != std::string::npos);
	return 1;
});

/*
 * A password the hub refused is cleared, and the clearing reaches the file
 * the same way the password did. A hand-edited one that turns out to be
 * wrong does not survive being told so.
 */
EXO_TEST(dchubsession_a_refused_password_is_cleared_from_the_session_file,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string line;
	s.say("$GetPass");
	EXO_ASSERT(s.hub.expect("$MyPass", line));

	s.session->sendPassword("wrong-one");
	EXO_ASSERT(readFile(sessionFile.c_str()).find("wrong-one") != std::string::npos);

	s.say("$BadPass");
	EXO_ASSERT(readFile(sessionFile.c_str()).find("wrong-one") == std::string::npos);
	return 1;
});

/* A password nobody asked for is not sent. */
EXO_TEST(dchubsession_an_unsolicited_password_is_not_sent,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.session->sendPassword("unsolicited");
	pump([] { return false; }, 100);

	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("$MyPass") == std::string::npos);
	return 1;
});

/* A password the hub refused is forgotten, reported, and can be replaced. */
EXO_TEST(dchubsession_a_refused_password_is_reported_and_can_be_replaced,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string line;

	s.say("$GetPass");
	EXO_ASSERT(s.hub.expect("$MyPass", line));   /* the empty one */

	s.session->sendPassword("wrong");
	EXO_ASSERT(s.hub.expect("$MyPass ", line));
	EXO_ASSERT(line == "$MyPass wrong");

	s.listener.authRequested = false;
	s.say("$BadPass");
	EXO_ASSERT(s.listener.authRequested);

	/* Still in the password state, so a second attempt goes through. */
	s.session->sendPassword("right");
	EXO_ASSERT(s.hub.expect("$MyPass ", line));
	EXO_ASSERT(line == "$MyPass right");
	return 1;
});

EXO_TEST(dchubsession_the_session_notices_the_hub_going_away,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(s.session->isLoggedIn());

	s.hub.peer.reset();
	EXO_ASSERT(pump([&] { return !s.session->isLoggedIn(); }));
	return 1;
});

/* ---- lines a hub sent, at the length the hub chose to send them ---- */

/*
 * $MyINFO read its nickname from a fixed offset without consulting the
 * length at all, so a line this short was made from whatever followed it,
 * and the mode flag and the '$' after the description were read from beyond
 * the end of the line as well.
 */
EXO_TEST(dchubsession_a_myinfo_that_stops_early_is_refused,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);

	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO"));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO "));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL"));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL "));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL alice"));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL alice x$"));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL alice x$ "));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL alice x$ $"));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL alice x$ $DSL"));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL alice x$ $DSL$m@e"));
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL alice x$ $DSL$m@e$1024"));

	/* A mode field longer than the one byte the syntax allows for it. */
	EXO_ASSERT(!h.call("$MyINFO", "$MyINFO $ALL alice x$AP$DSL$m@e$1024$"));

	QuickDC::UserManager* users = s.session->getUserManager();
	EXO_ASSERT(users && users->getUser("alice") == 0);
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

EXO_TEST(dchubsession_a_whole_myinfo_is_read_as_it_was,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);
	QuickDC::UserManager* users = s.session->getUserManager();

	EXO_ASSERT(h.call("$MyINFO",
		"$MyINFO $ALL alice a description$ $DSL\x01$mail@example$1024$"));

	const QuickDC::User* alice = users ? users->getUser("alice") : 0;
	EXO_ASSERT(alice != 0);
	EXO_ASSERT(alice && alice->getDescription()
		&& std::string(alice->getDescription()) == "a description");
	EXO_ASSERT(alice && alice->getEmail()
		&& std::string(alice->getEmail()) == "mail@example");
	EXO_ASSERT(alice && alice->getSharedBytes() == 1024);

	/* The tag is the last "<...>" of the description, and not part of it. */
	EXO_ASSERT(h.call("$MyINFO",
		"$MyINFO $ALL bob shared<++ V:0.1,M:A,H:1/0/0,S:2>$ $DSL\x01$b@e$2048$"));

	const QuickDC::User* bob = users ? users->getUser("bob") : 0;
	EXO_ASSERT(bob && bob->getDescription()
		&& std::string(bob->getDescription()) == "shared");
	EXO_ASSERT(bob && bob->getNumSlots() == 2);
	EXO_ASSERT(bob && bob->getUserAgent()
		&& std::string(bob->getUserAgent()) == "DC++ 0.1");
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/*
 * $ConnectToMe with nothing in front of the address: the nick was taken from
 * a fixed offset to a separator that was never found, so its length was
 * measured from address zero.
 */
EXO_TEST(dchubsession_connecttome_needs_a_nick_as_well_as_an_address,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);
	s.say("$Hello alice");

	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe 1.2.3.4:411"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe "));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick :411"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4:"));

	/*
	 * A port is sixteen bits and the hub picks the number. Truncating
	 * one that does not fit dials somewhere else entirely - 70000 lands
	 * on 4464 - so a port that cannot be read refuses the command.
	 */
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4:70000"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4:65536"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4:0"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4:-1"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4:411x"));
	EXO_ASSERT(!h.call("$ConnectToMe", "$ConnectToMe nick 1.2.3.4:abc"));
	EXO_ASSERT(s.listener.connectTo.empty());

	/* The largest one that does fit is still accepted. */
	EXO_ASSERT(h.call("$ConnectToMe", "$ConnectToMe " + s.nick + " 127.0.0.1:65535"));
	EXO_ASSERT(s.listener.connectTo.size() == 1
		&& s.listener.connectTo[0] == "127.0.0.1:65535");
	s.listener.connectTo.clear();
	s.listener.connectToKnown = 0;

	/* One nick is ours, and the hub is not saying who is calling. */
	EXO_ASSERT(h.call("$ConnectToMe", "$ConnectToMe " + s.nick + " 127.0.0.1:411"));
	EXO_ASSERT(s.listener.connectTo.size() == 1);
	EXO_ASSERT(!s.listener.connectTo.empty()
		&& s.listener.connectTo[0] == "127.0.0.1:411");
	EXO_ASSERT(s.listener.connectToKnown == 0);

	/* Two, and the first of them is the peer that asked. */
	EXO_ASSERT(h.call("$ConnectToMe", "$ConnectToMe alice " + s.nick + " 127.0.0.1:412"));
	EXO_ASSERT(s.listener.connectTo.size() == 2);
	EXO_ASSERT(s.listener.connectTo.size() == 2
		&& s.listener.connectTo[1] == "127.0.0.1:412");
	EXO_ASSERT(s.listener.connectToKnown == 1);
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/*
 * $Search stopping inside its pattern. The size, the type and the query were
 * all read from fixed offsets past the end, and the query that came out of
 * it was then matched against the share.
 */
EXO_TEST(dchubsession_a_search_stopping_short_of_its_query_is_refused,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);

	EXO_ASSERT(!h.call("$Search", "$Search 1.2.3.4:1 T?T?"));
	EXO_ASSERT(!h.call("$Search", "$Search 1.2.3.4:1 T?T?0?"));
	EXO_ASSERT(!h.call("$Search", "$Search 1.2.3.4:1 T?T"));
	EXO_ASSERT(!h.call("$Search", "$Search 1.2.3.4:1"));
	EXO_ASSERT(!h.call("$Search", "$Search"));

	/* Too short to be either an address or a "Hub:{nick}". */
	EXO_ASSERT(!h.call("$Search", "$Search abc F?F?0?1?x"));

	/* A flag of more than the single character it is. */
	EXO_ASSERT(!h.call("$Search", "$Search 1.2.3.4:1 TT?T?0?1?x"));

	EXO_ASSERT(h.call("$Search", "$Search 1.2.3.4:1 F?T?0?1?word"));
	EXO_ASSERT(h.call("$Search", "$Search 1.2.3.4:1 T?F?1024?1?two$terms"));
	EXO_ASSERT(h.call("$Search",
		"$Search Hub:alice F?F?0?9?TTH:2ZROV7YW7RTOKFJDF62WDMM2XGGC55Q3VF7WODI"));
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/*
 * $Supports of exactly ten bytes: the prefix was taken off by length
 * without checking there was one, leaving nothing, and the last character
 * of nothing was then looked at.
 */
EXO_TEST(dchubsession_a_supports_naming_nothing_is_not_an_empty_string_read,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);

	EXO_ASSERT(!h.call("$Supports", "$Supports"));
	EXO_ASSERT(h.call("$Supports", "$Supports "));
	EXO_ASSERT(h.call("$Supports", "$Supports NoGetINFO UserIP2 "));
	EXO_ASSERT(h.call("$Supports", "$Supports OpPlus"));
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/*
 * $UserIP had no length check whatever, and took its prefix off with
 * substr(), which throws rather than returning anything - and nothing on the
 * way here catches.
 */
EXO_TEST(dchubsession_userip_needs_the_prefix_it_takes_off,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);
	s.say("$MyINFO $ALL alice x$ $DSL\x01$m@e$100$");
	s.say("$MyINFO $ALL bob y$ $DSL\x01$m@e$200$");

	EXO_ASSERT(!h.call("$UserIP", "$UserIP"));
	EXO_ASSERT(!h.call("$UserIP", "$UserI"));
	EXO_ASSERT(!h.call("$UserIP", ""));

	const int before = s.listener.userUpdates;
	EXO_ASSERT(h.call("$UserIP", "$UserIP alice 10.0.0.1"));
	EXO_ASSERT(s.listener.userUpdates == before + 1);

	/*
	 * Both entries of the list are reached, which is what the count is here to
	 * show. Each names an address its user did not already have, since one that
	 * changes nothing is not reported.
	 */
	EXO_ASSERT(h.call("$UserIP", "$UserIP alice 10.0.0.3$$bob 10.0.0.2$$"));
	EXO_ASSERT(s.listener.userUpdates == before + 3);
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/*
 * $To: where the " $" that ends the sender's nick is inside the " From: "
 * that was just matched, so the nick was a field of negative length.
 */
EXO_TEST(dchubsession_a_to_with_no_message_is_refused,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);
	s.say("$Hello alice");

	EXO_ASSERT(!h.call("$To:", "$To: aaa From: $x"));
	EXO_ASSERT(!h.call("$To:", "$To:"));
	EXO_ASSERT(!h.call("$To:", "$To: me From: alice"));
	EXO_ASSERT(!h.call("$To:", "$To: me From: "));
	EXO_ASSERT(s.listener.privateChat.empty());

	EXO_ASSERT(h.call("$To:", "$To: " + s.nick + " From: alice $hello there"));
	EXO_ASSERT(s.listener.privateChat.size() == 1);
	EXO_ASSERT(!s.listener.privateChat.empty()
		&& s.listener.privateChat[0] == "hello there");

	/* A bot that repeats the sender's nick in the message body. */
	EXO_ASSERT(h.call("$To:", "$To: " + s.nick + " From: alice $<alice> hi"));
	EXO_ASSERT(s.listener.privateChat.size() == 2);
	EXO_ASSERT(s.listener.privateChat.size() == 2
		&& s.listener.privateChat[1] == "hi");
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/*
 * $Lock with the "Pk=" immediately after the keyword: the lock ran from
 * after the prefix to before it, a negative length. $RevConnectToMe naming
 * one nick where the syntax has two does the same.
 */
EXO_TEST(dchubsession_a_lock_and_a_revconnecttome_with_a_field_missing_are_refused,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);
	s.say("$Hello alice");

	EXO_ASSERT(!h.call("$Lock", "$Lock Pk=abcdef"));
	EXO_ASSERT(!h.call("$Lock", "$Lock"));
	EXO_ASSERT(!h.call("$Lock", "$Lock abc"));
	EXO_ASSERT(!h.call("$Lock", "$Lock abc Pk="));
	EXO_ASSERT(h.call("$Lock", "$Lock " HUB_LOCK " Pk=TESTHUB"));

	EXO_ASSERT(!h.call("$RevConnectToMe", "$RevConnectToMe alice"));
	EXO_ASSERT(!h.call("$RevConnectToMe", "$RevConnectToMe"));
	EXO_ASSERT(!h.call("$RevConnectToMe", "$RevConnectToMe  alice"));

	const int before = s.listener.passiveConnect;
	EXO_ASSERT(h.call("$RevConnectToMe", "$RevConnectToMe alice " + s.nick));
	EXO_ASSERT(s.listener.passiveConnect == before + 1);
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/* The rest of what a hub can say, with and without its arguments. */
EXO_TEST(dchubsession_every_other_command_needs_the_arguments_it_reads,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);
	QuickDC::UserManager* users = s.session->getUserManager();

	EXO_ASSERT(!h.call("$Hello", "$Hello"));
	EXO_ASSERT(!h.call("$Hello", "$Hello "));
	EXO_ASSERT(h.call("$Hello", "$Hello carol"));
	EXO_ASSERT(users && users->getUser("carol") != 0);

	EXO_ASSERT(!h.call("$HubName", "$HubName"));
	EXO_ASSERT(!h.call("$HubName", "$HubName "));
	EXO_ASSERT(h.call("$HubName", "$HubName Some Hub"));
	EXO_ASSERT(s.listener.hubName == "Some Hub");

	EXO_ASSERT(!h.call("$Quit", "$Quit"));
	EXO_ASSERT(!h.call("$Quit", "$Quit "));
	EXO_ASSERT(h.call("$Quit", "$Quit carol"));
	EXO_ASSERT(users && users->getUser("carol") == 0);

	EXO_ASSERT(!h.call("$ForceMove", "$ForceMove"));
	EXO_ASSERT(!h.call("$ForceMove", "$ForceMove "));
	EXO_ASSERT(h.call("$ForceMove", "$ForceMove otherhub:412"));
	EXO_ASSERT(s.listener.redirects.size() == 1);
	EXO_ASSERT(!s.listener.redirects.empty()
		&& s.listener.redirects[0] == "otherhub:412");

	/* Given but unreadable is a broken command, where absent is not. */
	EXO_ASSERT(!h.call("$ForceMove", "$ForceMove otherhub:70000"));
	EXO_ASSERT(!h.call("$ForceMove", "$ForceMove otherhub:0"));
	EXO_ASSERT(!h.call("$ForceMove", "$ForceMove otherhub:abc"));
	EXO_ASSERT(!h.call("$ForceMove", "$ForceMove otherhub:"));
	EXO_ASSERT(s.listener.redirects.size() == 1);

	/* No port named, and 411 is the default in DC. */
	EXO_ASSERT(h.call("$ForceMove", "$ForceMove otherhub"));
	EXO_ASSERT(s.listener.redirects.size() == 2);
	EXO_ASSERT(s.listener.redirects.size() == 2
		&& s.listener.redirects[1] == "otherhub:411");

	/* A list of nicks is nothing to refuse over, so these say so. */
	EXO_ASSERT(h.call("$NickList", "$NickList"));
	EXO_ASSERT(h.call("$NickList", "$NickList a$$b$$"));
	EXO_ASSERT(h.call("$OpList", "$OpList"));
	EXO_ASSERT(h.call("$OpList", "$OpList alice$$"));

	EXO_ASSERT(!h.call("PublicChat", "<"));
	EXO_ASSERT(!h.call("PublicChat", "<alice>"));
	EXO_ASSERT(!h.call("PublicChat", "<> hello"));
	EXO_ASSERT(h.call("PublicChat", "<alice> hello"));

	EXO_ASSERT(!h.call("ActionChat", "*"));
	EXO_ASSERT(!h.call("ActionChat", "* alice"));
	EXO_ASSERT(h.call("ActionChat", "* alice waves"));
	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/*
 * Every prefix of a well formed line of each kind, which is the general
 * case of all of the above: a handler that indexes a fixed offset reads past
 * the end of the line for some length of every one of these.
 */
EXO_TEST(dchubsession_no_handler_reads_past_the_length_it_was_given,
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
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Handlers h(s);
	s.say("$Hello alice");

	for (size_t n = 0; n < sizeof(lines)/sizeof(lines[0]); n++)
		for (size_t len = 0; len <= strlen(lines[n].line); len++)
			h.call(lines[n].name, std::string(lines[n].line, len));

	EXO_ASSERT(everyHandlerFound(h));
	return 1;
});

/* Sessions come and go without leaving anything in the monitor. */
EXO_TEST(dchubsession_repeated_sessions_leave_nothing_behind,
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();
	const size_t baseline = monitor->size();

	for (int n = 0; n < 3; n++)
	{
		Session s;
		if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
		EXO_ASSERT(s.ready);
		if (s.ready) s.login();
	}

	pump([&] { return monitor->size() <= baseline; }, 2000);
	EXO_ASSERT(monitor->size() == baseline);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The password goes out in the clear                                        */
/* ------------------------------------------------------------------------- */

namespace {

void setAllowPlaintextPasswords(bool allowed)
{
	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->setBool("Allow plaintext passwords", allowed);
}

void forgetPlaintextPasswordSetting()
{
	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->remove("Allow plaintext passwords");
}

bool warnedAboutPlaintext(const Listener& listener)
{
	for (const std::string& e : listener.errors)
		if (e.find("clear") != std::string::npos) return true;
	return false;
}

}

/*
 * NMDC puts the password on the wire as typed - there is no hashed exchange in
 * the protocol and no TLS path for it anywhere here - and from the client's side
 * an NMDC hub asking to log in looks exactly like an ADC hub asking to log in.
 * Saying so is the whole of what can be done about it.
 */
EXO_TEST(dchubsession_sending_a_password_in_the_clear_is_reported,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	setAllowPlaintextPasswords(true);

	std::string line;
	s.say("$GetPass");
	EXO_ASSERT(s.hub.expect("$MyPass", line));

	s.session->sendPassword("hunter2");
	EXO_ASSERT(s.hub.expect("$MyPass ", line));
	EXO_ASSERT(line == "$MyPass hunter2");

	/* Sent, and the user was told what sending it means. */
	EXO_ASSERT(warnedAboutPlaintext(s.listener));
	return 1;
});

/* And a user who would rather not can say so, in which case it is not sent. */
EXO_TEST(dchubsession_a_password_is_withheld_when_plaintext_is_refused,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	setAllowPlaintextPasswords(false);

	std::string line;
	s.say("$GetPass");

	/* The empty answer still goes, carrying nothing. */
	EXO_ASSERT(s.hub.expect("$MyPass", line));
	s.hub.received.clear();

	s.session->sendPassword("hunter2");

	/*
	 * Nothing with the password in it follows, and the user is told why. Pumped
	 * first, because sendPassword() is called here rather than through say():
	 * drain() reads what has arrived, and nothing arrives without the loop
	 * running - so without this the assertion would hold whether the password
	 * was sent or not.
	 */
	pump([] { return false; }, 400);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("hunter2") == std::string::npos);
	EXO_ASSERT(warnedAboutPlaintext(s.listener));

	setAllowPlaintextPasswords(true);
	return 1;
});

/*
 * The default, which is to send it. There is nothing to fall back to on NMDC, so
 * refusing by default would end every registered login without offering a way to
 * keep one - the warning is what changes, not whether logging in works.
 */
EXO_TEST(dchubsession_a_password_is_sent_by_default,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	forgetPlaintextPasswordSetting();

	std::string line;
	s.say("$GetPass");
	EXO_ASSERT(s.hub.expect("$MyPass", line));

	s.session->sendPassword("hunter2");
	EXO_ASSERT(s.hub.expect("$MyPass ", line));
	EXO_ASSERT(line == "$MyPass hunter2");
	return 1;
});

/* An empty password carries no secret, so there is nothing to warn about. */
EXO_TEST(dchubsession_an_empty_password_is_not_reported_as_plaintext,
{
	Session s;
	if (!s.listened) EXO_SKIP("could not bind a loopback port for the hub");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	setAllowPlaintextPasswords(true);

	std::string line;
	s.say("$GetPass");
	EXO_ASSERT(s.hub.expect("$MyPass", line));

	EXO_ASSERT(!warnedAboutPlaintext(s.listener));
	return 1;
});
