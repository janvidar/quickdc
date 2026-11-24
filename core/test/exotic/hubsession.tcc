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
#include "chat/archive.h"
#include "api/hub.h"
#include "network/hubmanager.h"
#include "api/server.h"
#include "network/reachability.h"
#include <samurai/io/net/datagram.h>
#include "network/adc/adcudp.h"
#include "network/adc/parser.h"
#include "network/searchtokens.h"
#include "network/downloadqueue.h"
#include "network/filesource.h"
#include "api/attachments.h"
#include "hash/hashmanager.h"
#include "share/sharemanager.h"

#include "fakegateway.h"
#include "network/adc/adccommon.h"
#include "network/adc/client/adchubsession.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "config/preferences.h"

#include <samurai/io/dir.h>
#include <samurai/io/file.h>
#include <samurai/io/net/tlsfactory.h>
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

/* The fixture's core; built in EXO_SETUP below. */
QuickDC::Core* core = 0;

static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_hubtest_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/* Where the core under test keeps its session data. */
static std::string session_file;

/* And its chat transcripts. */
static std::string chat_dir;
static std::string queue_file;

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

		/** Each user update reported, as the nick and what it said about away. */
		std::vector<std::pair<std::string, bool> > updated;
		std::string hubName;
		std::string hubDescription;
		int netConnected = 0;
		int loggedIn = 0;
		bool authRequested = false;

		void EventNetStatus(enum StatusNetwork state) override
		{ if (state == NetConnected) netConnected++; }

		void EventHubStatus(enum StatusHub state) override
		{ if (state == HubLoggedIn) loggedIn++; }

		void EventSystemError(const char* msg) override
		{ errors.push_back(msg ? msg : ""); }

		void EventChat(const QuickDC::User*, const char* message, bool, bool,
		               uint64_t) override
		{ chat.push_back(message ? message : ""); }

		void EventPrivateChat(const QuickDC::User*, const QuickDC::User*,
			const char* message, const char*, bool, bool, uint64_t) override
		{ privateChat.push_back(message ? message : ""); }

		void EventHubMessage(const char*, const char* message, bool,
			bool richText, uint64_t) override
		{
			hubMessages.push_back(message ? message : "");
			hubMarkdown.push_back(richText);
		}

		void EventUserJoin(const char* user) override
		{ joined.push_back(user ? user : ""); }

		void EventUserLeave(const QuickDC::User* user, const char*, bool) override
		{ left.push_back(user && user->getNick() ? user->getNick() : ""); }

		void EventUsersCleanup() override { }

		/*
		 * Nick and away for every update reported, so that a test can tell not only
		 * what the user record ended up holding but whether the interface was ever
		 * told - a change nobody is told about is a change no user list draws.
		 */
		void EventUserUpdate(const QuickDC::User* user) override
		{
			updated.push_back(std::make_pair(
				user && user->getNick() ? user->getNick() : "",
				user && user->isAway()));
		}

		void EventSearchResult(void*) override { }

		void EventHubName(const char* name) override
		{ hubName = name ? name : ""; }

		void EventHubDescription(const char* description) override
		{ hubDescription = description ? description : ""; }

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

		/**
		 * @param secure what the session believes it is running over. The channel
		 *        is plain unless a case asks otherwise - nothing here speaks TLS,
		 *        and the flag only decides whether answering a password request is
		 *        reported as crossing the clear.
		 * @param tail anything after the authority - a path, a query. Empty for
		 *        every case but the one about what a hub's settings are filed
		 *        under, which is the one place the difference is the point.
		 */
		explicit Session(bool secure = false,
		                 const std::string& tail = std::string())
		{
			if (!hub.listen()) return;

			const std::string address =
				"adc://127.0.0.1:" + std::to_string(hub.port()) + tail;
			url = std::make_unique<Samurai::IO::Net::URL>(address);

			ui = std::make_unique<QuickDC::Hub>(*core, &listener, url.get());
			session = new ADC::HubSession(ui.get(), &listener, url.get(), secure);

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
 * The fixture. One core for the whole process, because it runs on samurai's
 * process-wide reactor, is expensive to build, and has to be pointed at scratch files rather than the
 * user's own configuration.
 */
std::string config_file;

/*
 * The value of one INF field, or nothing when the field is absent.
 *
 * Looked up by token rather than by searching the line, because an INF carries a
 * CID and a PID as thirty-nine characters of base32 each - and base32 is letters
 * and digits, so "U4", "TCP4" and "UDP4" all occur inside one from time to time.
 * Those identifiers are random per run, which makes a search for a bare field name
 * a test that passes most days.
 */
std::string infField(const std::string& line, const std::string& field)
{
	size_t at = 0;
	while (at < line.size())
	{
		size_t end = line.find(' ', at);
		if (end == std::string::npos) end = line.size();

		const std::string token = line.substr(at, end - at);
		if (token.compare(0, field.size(), field) == 0)
			return token.substr(field.size());

		at = end + 1;
	}
	return std::string();
}

/** Whether the SU field lists a flag, which is a comma separated set. */
bool claims(const std::string& line, const std::string& flag)
{
	const std::string su = infField(line, "SU");

	size_t at = 0;
	while (at < su.size())
	{
		size_t end = su.find(',', at);
		if (end == std::string::npos) end = su.size();

		if (su.compare(at, end - at, flag) == 0) return true;
		at = end + 1;
	}
	return false;
}

/** Whether an INF carries a field at all. */
bool hasField(const std::string& line, const std::string& field)
{
	size_t at = 0;
	while (at < line.size())
	{
		size_t end = line.find(' ', at);
		if (end == std::string::npos) end = line.size();

		if (line.compare(at, field.size(), field) == 0) return true;
		at = end + 1;
	}
	return false;
}

}

EXO_SETUP({
	config_file = scratch("config.conf");
	session_file = scratch("session.conf");

	/*
	 * With a transcript directory, so that what the session records as chat
	 * arrives can be read back - see the cases at the foot of this file.
	 */
	chat_dir = scratch("chat");

	/*
	 * And with a download queue, which the source search cases need: a search for a
	 * file is sent from the queue, and a queue is only built when it is given
	 * somewhere to keep itself. Nothing else here reads it, and nothing drives it -
	 * these cases pump the socket monitor rather than running the core.
	 */
	queue_file = scratch("queue.xml");

	core = new QuickDC::Core(config_file, session_file, queue_file, chat_dir);
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
	Samurai::IO::File(queue_file).remove();

	/*
	 * And the transcripts. Emptied rather than removed by name, because every
	 * case that says anything on a hub writes one now - the chat cases above
	 * were not written with a transcript in mind and should not have to be.
	 */
	Samurai::IO::Directory dir(chat_dir);

	if (dir.open())
	{
		for (auto entry = dir.first(); entry; entry = dir.next())
			Samurai::IO::File(entry->getName()).remove();

		dir.close();
	}

	Samurai::IO::File::rmdir(chat_dir.c_str());
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

	/*
	 * And PING is not among them. It says the sender answers a pinger asking what
	 * hub this is and who is on it, which is something only a hub does - a client
	 * claiming it advertises a service nothing here runs.
	 */
	EXO_ASSERT(line.find("AD" ADC_EXT_PING) == std::string::npos);
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
 * A field the user left blank is left out, rather than sent as a name with
 * nothing after it.
 *
 * Nobody has to read it either way, so this is about what the client says about
 * itself: an empty 'EM' is a claim to have an e-mail address that happens to be
 * the empty string, and ADC reads an empty field as an instruction to drop one -
 * which is a strange thing to open a conversation with.
 */
EXO_TEST(hubsession_an_unset_email_is_not_announced,
{
	QuickDC::Preferences* config = core->config;
	config->setGroup("User");
	config->setString("E-mail", "");

	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("ISUP ADBASE ADTIGR");
	s.say("ISID AAAA");

	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "BINF") == 0);

	EXO_ASSERT(!hasField(line, "EM"));

	/* The description is configured, so that one is there - which is what makes
	   the absence above about the value and not about the field. */
	EXO_ASSERT(hasField(line, "DE"));
	return 1;
});

/* And one the user did fill in is announced. */
EXO_TEST(hubsession_a_configured_email_is_announced,
{
	QuickDC::Preferences* config = core->config;
	config->setGroup("User");
	config->setString("E-mail", "someone@example.com");

	Session s;
	const bool listening = s.hub.listening;

	std::string line;
	bool got = false;
	if (listening && s.ready)
	{
		got = s.hub.nextLine(line);
		s.say("ISUP ADBASE ADTIGR");
		s.say("ISID AAAA");
		got = got && s.hub.nextLine(line);
	}

	/* Put back before anything is asserted, since the core outlives this case
	   and a failed assertion leaves. */
	config->setGroup("User");
	config->setString("E-mail", "");

	if (!listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(got);
	EXO_ASSERT(line.compare(0, 4, "BINF") == 0);
	EXO_ASSERT(infField(line, "EM") == "someone@example.com");
	return 1;
});

/*
 * A status update re-sends INF, which is how a hub is told that something it
 * was told once has changed - the port a peer should use being the case that
 * matters, since forwarding it succeeds long after the hub was joined.
 *
 * And it says what changed and nothing else. A hub merges an INF into what it
 * holds and relays the fields that arrived, so repeating the rest costs a line
 * the length of the whole record and buys nothing - and 'PD', which proves the
 * client ID, may only be sent once.
 */
EXO_TEST(hubsession_a_status_update_says_only_what_changed,
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

	/* Something the hub was told once and is no longer true. */
	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->setNumber("Slots", 17);

	s.session->sendClientInfo();
	pump([] { return false; }, 60);

	config->setGroup("Network");
	config->setNumber("Slots", 3);

	EXO_ASSERT(s.hub.nextLine(line));

	if (line.compare(0, 4, "BINF") != 0)
	{
		const std::string why = "the update was not an INF: " + line;
		EXO_FAIL(why.c_str());
	}

	EXO_ASSERT(infField(line, "SL") == "17");

	/* The nick did not change, and the two identity fields never do. */
	EXO_ASSERT(!hasField(line, "NI"));
	EXO_ASSERT(!hasField(line, "ID"));
	EXO_ASSERT(!hasField(line, "PD"));
	EXO_ASSERT(!hasField(line, "VE"));
	return 1;
});

/*
 * And an update with nothing to report is not sent at all, rather than sent as
 * a line naming this client and no fields. Every hub count, port forwarding
 * result and reachability verdict asks for one, and a hub relays each of them
 * to everyone on it.
 */
EXO_TEST(hubsession_an_update_of_nothing_is_not_sent,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.hub.drain();
	s.hub.received.clear();

	s.session->sendClientInfo();
	pump([] { return false; }, 120);

	s.hub.drain();
	EXO_ASSERT(s.hub.received.empty());
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

	QuickDC::HubManager* hubs = core->hubs;
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
	QuickDC::Hub ui(*core, &listener, &url);

	QuickDC::HubManager* hubs = core->hubs;
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

	/* Something for the update to be about, since one with nothing to report
	   is not sent - which is what the case after this one is for. */
	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->setNumber("Slots", 11);

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
	config->setGroup("Network");
	config->setNumber("Slots", 3);

	EXO_ASSERT(arrived);
	EXO_ASSERT(!pending);
	EXO_ASSERT(infs == 1);
	return 1;
});

/*
 * A hub that joined and had nothing change since is told nothing, which is what
 * makes the pre-empted hub count worth pre-empting: the update login schedules
 * finds the counts already right and goes no further.
 *
 * Driven through Hub::connect() like the case above, because it is the count
 * this session contributes to the manager that is under test, and the manager
 * only sees a session connect() made.
 */
EXO_TEST(hubsession_the_login_inf_already_counts_this_hub,
{
	FakeHub fake;
	Listener listener;
	if (!fake.listen()) EXO_SKIP("could not listen on a loopback port");

	const std::string address = "adc://127.0.0.1:" + std::to_string(fake.port());
	Samurai::IO::Net::URL url(address);
	QuickDC::Hub ui(*core, &listener, &url);

	QuickDC::HubManager* hubs = core->hubs;

	/* Whatever earlier cases left behind, which this one is measured against. */
	const size_t before = hubs->countHubsNormal();

	ui.connect();
	EXO_ASSERT(fake.accepted());

	std::string line;
	EXO_ASSERT(fake.nextLine(line));
	fake.send("ISUP ADBASE ADTIGR");
	pump([] { return false; }, 60);
	fake.send("ISID AAAA");
	pump([] { return false; }, 60);

	EXO_ASSERT(fake.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "BINF") == 0);

	/* One more than the hubs already joined: this one, counted before the hub
	   had a chance to say so. */
	EXO_ASSERT(infField(line, "HN") == std::to_string(before + 1));

	/* And logging in changes nothing, so the update it asks for says nothing. */
	fake.send(line);
	pump([] { return false; }, 60);
	fake.drain();
	fake.received.clear();

	hubs->sendClientInfoToAllHubs();
	pump([] { return false; }, 120);
	fake.drain();
	EXO_ASSERT(fake.received.find("BINF") == std::string::npos);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* what the hub says it supports                                             */
/* ------------------------------------------------------------------------- */

/* The features an ISUP names are what the session will ask the hub for. */
EXO_TEST(hubsession_the_hubs_features_are_read_off_its_sup,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("ISUP ADBASE ADTIGR AD" ADC_EXT_RICHTEXT);

	EXO_ASSERT(s.session->supports("BASE"));
	EXO_ASSERT(s.session->supports(ADC_HASH_TIGER));
	EXO_ASSERT(s.session->supportsRichText());

	/* One the hub did not name, and a name that is not there at all - which is a
	   question the session has to be able to answer without reading through it. */
	EXO_ASSERT(!s.session->supports(ADC_EXT_PING));
	EXO_ASSERT(!s.session->supports(0));
	return 1;
});

/*
 * A hub may take a feature back, by naming it with 'RM' in a later ISUP, and the
 * answer follows what it last said in both directions.
 *
 * Withdrawn once the session is up, which is the case that matters: the hub is
 * the only one who knows whether it can still serve a feature, and it may lose
 * whatever it needed for one at any point - after which a client that goes on
 * asking is one it has to refuse for the rest of the session.
 */
EXO_TEST(hubsession_a_feature_the_hub_withdraws_is_no_longer_supported,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	/* The login exchange by hand, since it is the ISUP of it that has to carry
	   the feature this case takes away again. */
	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("ISUP ADBASE ADTIGR AD" ADC_EXT_RICHTEXT);
	s.say("ISID AAAA");
	s.say("IINF NITestHub");

	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "BINF") == 0);
	s.say(line);

	EXO_ASSERT(s.session->isLoggedIn());
	EXO_ASSERT(s.session->supportsRichText());

	s.say("ISUP RM" ADC_EXT_RICHTEXT);

	EXO_ASSERT(!s.session->supportsRichText());

	/* Only the one it named. */
	EXO_ASSERT(s.session->supports("BASE"));
	EXO_ASSERT(s.session->supports(ADC_HASH_TIGER));

	/* And a withdrawal is no more permanent than the offer was. */
	s.say("ISUP AD" ADC_EXT_RICHTEXT);
	EXO_ASSERT(s.session->supportsRichText());
	return 1;
});

/*
 * How many features one hub may name.
 *
 * The set is fed by the hub and held for the life of the connection, so a hub
 * that keeps sending ISUP is the one deciding how much this client remembers.
 */
EXO_TEST(hubsession_a_hub_can_only_name_so_many_features,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("ISUP ADBASE ADTIGR");

	/* A hundred names no extension has ever had, which is more than the ceiling
	   and fewer than one command may carry. */
	std::string flood("ISUP");
	for (int n = 0; n < 100; n++)
	{
		flood += " AD";
		flood += static_cast<char>('A' + (n / 26));
		flood += static_cast<char>('A' + (n % 26));
		flood += "ZZ";
	}
	s.say(flood);

	/* The ceiling has been reached, so what arrives after it is not taken - while
	   what the hub said before it is still there. */
	s.say("ISUP ADZZZZ");
	EXO_ASSERT(!s.session->supports("ZZZZ"));
	EXO_ASSERT(s.session->supports("BASE"));
	EXO_ASSERT(s.session->supports(ADC_HASH_TIGER));
	return 1;
});

/*
 * A feature named once the session is over is not taken either. There is no hub
 * left to ask for it, and the set is what this session answers for.
 */
EXO_TEST(hubsession_a_feature_named_after_the_session_ended_is_ignored,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.hub.peer.reset();
	EXO_ASSERT(pump([&] { return !s.session->isLoggedIn(); }));

	s.session->addSupport("ZZZZ");
	EXO_ASSERT(!s.session->supports("ZZZZ"));
	return 1;
});

/*
 * And nothing is remembered against the hub itself: a session that comes back
 * knows only what this hub has said this time.
 *
 * Each connection is a HubSession of its own - QuickDC::Hub builds one per
 * attempt - so the set cannot outlive the connection it was negotiated on. A
 * client that cached it would offer to use a feature the hub has since stopped
 * serving, on the strength of a conversation that has ended.
 */
EXO_TEST(hubsession_a_new_session_learns_the_features_over_again,
{
	{
		Session s;
		if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
		EXO_ASSERT(s.ready);

		std::string line;
		EXO_ASSERT(s.hub.nextLine(line));
		s.say("ISUP ADBASE ADTIGR AD" ADC_EXT_RICHTEXT);
		EXO_ASSERT(s.session->supportsRichText());
	}

	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	s.say("ISUP ADBASE ADTIGR");

	EXO_ASSERT(s.session->supports("BASE"));
	EXO_ASSERT(!s.session->supportsRichText());
	return 1;
});

/* ------------------------------------------------------------------------- */
/* who is on the hub                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Which section of session.conf a hub's settings live in.
 *
 * The canonical "{scheme}://{host}:{port}", not the address as it was typed. A
 * keyprint in the address says how to verify the hub, not which hub it is, so an
 * address carrying one has to land on the same settings as the address without -
 * otherwise pasting a longer address once silently abandons the nickname, the
 * password and everything else already set up for that hub.
 */
EXO_TEST(hubsession_settings_are_filed_under_the_canonical_address,
{
	/*
	 * A query rather than the 'kp' that prompted this: a keyprint is refused on an
	 * adc:// address, and rightly so - see connect() - while this fake hub speaks
	 * plain TCP. What is being tested is the rule rather than the keyprint, and the
	 * rule is that nothing after the authority names the hub.
	 */
	const std::string tail = "/?x=1";

	Session s(false, tail);
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	QuickDC::Preferences* store = core->session;
	EXO_ASSERT(store != 0);

	const std::string canonical =
		"adc://127.0.0.1:" + std::to_string(s.hub.port());

	/*
	 * The session records when it last connected as it is built; that is under the
	 * short form. (Its identity is read without being written back, so it is not
	 * there to look for.)
	 */
	store->setGroup(canonical);
	EXO_ASSERT(store->haveSetting("Last Connect"));

	/* And nothing at all is filed under the address as it was given. */
	store->setGroup(canonical + tail);
	EXO_ASSERT(!store->haveSetting("Nickname"));
	EXO_ASSERT(!store->haveSetting("Last Connect"));
	return 1;
});

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

/*
 * AW says a user is away, and an INF that does not mention it says nothing about
 * it - which is the part worth a test: an update carries only what changed, so
 * reading an absent AW as "back again" would clear the flag on every other change
 * a client makes while away.
 */
EXO_TEST(hubsession_the_away_flag_follows_inf_and_survives_an_unrelated_update,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice SS100");
	s.say("BINF CCCC NIbob SS200 AW1");
	s.say("BINF DDDD NIcarol SS300 AW2");

	QuickDC::UserManager* users = s.session->getUserManager();
	const QuickDC::User* alice = users ? users->getUser("BBBB") : 0;
	const QuickDC::User* bob = users ? users->getUser("CCCC") : 0;
	const QuickDC::User* carol = users ? users->getUser("DDDD") : 0;

	EXO_ASSERT(alice && !alice->isAway());
	EXO_ASSERT(bob && bob->isAway());

	/* Extended away is still away; the difference is the hub's business. */
	EXO_ASSERT(carol && carol->isAway());

	/* An update about something else leaves the flag where it was. */
	s.say("BINF CCCC SS900");
	EXO_ASSERT(bob && bob->isAway());
	EXO_ASSERT(bob && bob->getSharedBytes() == 900);

	/* Coming back is the field sent empty, which reads as not away. */
	s.say("BINF CCCC AW");
	EXO_ASSERT(bob && !bob->isAway());
	return 1;
});

/*
 * And the interface is told about it.
 *
 * Separate from the test above because the record being right is only half of it:
 * UserManager::info() reports an update only when something changed, so a going-away
 * that did not count as a change would leave every user list drawing the old figure
 * until the user happened to alter something else.
 */
EXO_TEST(hubsession_going_away_is_reported_to_the_listener_on_its_own,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice SS100");

	const size_t afterJoin = s.listener.updated.size();
	EXO_ASSERT(afterJoin > 0);
	EXO_ASSERT(!s.listener.updated.back().second);

	/* Nothing but AW, which is all a client sends when it steps out. */
	s.say("BINF BBBB AW1");

	EXO_ASSERT(s.listener.updated.size() == afterJoin + 1);
	EXO_ASSERT(s.listener.updated.back().first == "alice");
	EXO_ASSERT(s.listener.updated.back().second);

	/* And again on the way back. */
	s.say("BINF BBBB AW0");

	EXO_ASSERT(s.listener.updated.size() == afterJoin + 2);
	EXO_ASSERT(s.listener.updated.back().first == "alice");
	EXO_ASSERT(!s.listener.updated.back().second);

	/* Away again, and back again with the field sent empty rather than as a zero. */
	s.say("BINF BBBB AW1");
	EXO_ASSERT(s.listener.updated.back().second);

	s.say("BINF BBBB AW");
	EXO_ASSERT(!s.listener.updated.back().second);

	/*
	 * Nothing is asserted here about an INF that changes nothing. One of those still
	 * reports an update, and so does an INF carrying no AW at all, so the coalescing
	 * that UserManager::info() looks like it is doing is not there - which predates
	 * this and costs a redraw rather than a wrong answer.
	 */
	return 1;
});

/*
 * CT says what the hub takes a user for, and the operator bit of it is the other
 * thing a user list draws. Read for the same reasons AW is: an update carries only
 * what changed, so an absent CT must not read as a demotion.
 */
EXO_TEST(hubsession_the_operator_flag_follows_the_ct_field_of_an_inf,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice SS100");
	s.say("BINF CCCC NIbob SS200 CT4");
	s.say("BINF DDDD NIcarol SS300 CT2");

	QuickDC::UserManager* users = s.session->getUserManager();
	const QuickDC::User* alice = users ? users->getUser("BBBB") : 0;
	const QuickDC::User* bob = users ? users->getUser("CCCC") : 0;
	const QuickDC::User* carol = users ? users->getUser("DDDD") : 0;

	EXO_ASSERT(alice && !alice->isOp());
	EXO_ASSERT(bob && bob->isOp());

	/* Registered, which is a bit of its own and not the operator one. */
	EXO_ASSERT(carol && !carol->isOp());

	/* A hub owner sets the operator bit alongside their own. */
	s.say("BINF DDDD CT28");
	EXO_ASSERT(carol && carol->isOp());

	/* An update about something else leaves the flag where it was. */
	s.say("BINF CCCC SS900");
	EXO_ASSERT(bob && bob->isOp());

	/* And a CT without the bit is a demotion. */
	s.say("BINF CCCC CT2");
	EXO_ASSERT(bob && !bob->isOp());
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

/*
 * And it leaves everything it does not mention alone.
 *
 * An INF update names what changed and nothing else, which is what a hub relays
 * and what this client sends. Reading one as a whole picture of the user empties
 * every field it happens not to carry, so a peer that moved a file into its share
 * would lose its description, its address and - the one that does damage - the
 * 'SU' saying it can be connected to and can traverse a NAT. Nothing reports that;
 * transfers with that peer simply stop being arranged the way they were.
 */
EXO_TEST(hubsession_an_inf_update_leaves_the_fields_it_omits_alone,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice DEsomeone EMalice@example.com VEDC/1.0 SL4 "
	      "SS100 I410.0.0.1 U41411 SUADC0,TCP4,UDP4,NATT");

	s.say("BINF BBBB SS500");

	QuickDC::UserManager* users = s.session->getUserManager();
	const QuickDC::User* alice = users ? users->getUser("BBBB") : 0;
	EXO_ASSERT(alice);

	EXO_ASSERT(alice->getSharedBytes() == 500);

	EXO_ASSERT(alice->getDescription() && !strcmp(alice->getDescription(), "someone"));
	EXO_ASSERT(alice->getEmail() && !strcmp(alice->getEmail(), "alice@example.com"));
	EXO_ASSERT(alice->getUserAgent() && !strcmp(alice->getUserAgent(), "DC/1.0"));
	EXO_ASSERT(alice->getNumSlots() == 4);
	EXO_ASSERT(alice->getAddressIPv4() && !strcmp(alice->getAddressIPv4(), "10.0.0.1"));
	EXO_ASSERT(alice->getPortIPv4() == 1411);

	/* The one a connection is planned from. */
	EXO_ASSERT(alice->supports(ADC_EXT_TCP4));
	EXO_ASSERT(alice->supports(ADC_EXT_NATT));
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
	s.say("IINF NIMy\\sHub DEa\\sfriendly\\splace");

	EXO_ASSERT(s.listener.hubName == "My Hub");
	EXO_ASSERT(s.listener.hubDescription == "a friendly place");
	return 1;
});

/*
 * A hub changes its topic by sending the one field again. The name is not in that
 * INF, and an update that carried it away would leave the interface naming the hub
 * by its address a minute after it learned better.
 */
EXO_TEST(hubsession_a_hubs_description_can_change_without_its_name,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	s.hub.nextLine(line);
	s.say("ISUP ADBASE");
	s.say("ISID AAAA");
	s.say("IINF NIMy\\sHub DEfirst");

	EXO_ASSERT(s.listener.hubName == "My Hub");
	EXO_ASSERT(s.listener.hubDescription == "first");

	s.say("IINF DEsecond");

	EXO_ASSERT(s.listener.hubDescription == "second");
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

	s.say("IGPA ABCDEFGHIJKLMNOPQRSTUVWXYZ");
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

	s.say("IGPA ABCDEFGHIJKLMNOPQRSTUVWXYZ");
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

		s.say("IGPA ABCDEFGHIJKLMNOPQRSTUVWXYZ");
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

	s.say("IGPA ABCDEFGHIJKLMNOPQRSTUVWXYZ");
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

		/*
		 * Twelve doublings from five seconds is well past the ceiling. Half of it
		 * is the bound, and it is exclusive: a wait at the ceiling is spread over
		 * the upper half, so the smallest answer this can give is exactly half and
		 * that is still the ceiling being reached.
		 */
		if (QuickDC::Hub::retryDelay(reasons[r], 12) < QUICKDC_HUB_RETRY_MAX / 2)
		{
			const std::string why = which + " never reaches the ceiling";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Whether a peer transfer has to be encrypted                               */
/* ------------------------------------------------------------------------- */

/*
 * A peer transfer carries the file, and which protocol carries it used to be
 * decided by one field of a message the hub relayed - so a hub, or anyone able
 * to forge a CTM through one, chose whether the transfer that followed was
 * encrypted. "Require encrypted transfers" takes that choice away, and defaults
 * to on.
 *
 * These cases drive the two directions a peer connection is arranged in: a CTM
 * telling us to dial the peer, and an RCM asking us to invite the peer in.
 */

namespace {

/* The session under test is on a plain ADC hub, which is the interesting case:
   a plain hub may still broker an encrypted peer connection. */
void setRequireEncryption(bool required)
{
	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->setBool("Require encrypted transfers", required);
}

/* No setting at all, which is what a client that has never been configured has. */
void forgetEncryptionSetting()
{
	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->remove("Require encrypted transfers");
}

/*
 * A certificate for this client to present.
 *
 * The fixture core does not make one - it is the constructor meant for a test, and
 * it stays out of the user's own ssl directory - so everything this client will
 * only claim or accept when it could answer a TLS handshake is unavailable without
 * one, which is most of what encryption is negotiated for.
 *
 * Held for the length of a case and pointed away again afterwards, so that the
 * cases about what a client with no certificate says still see none.
 */
class Certificate
{
	public:
		Certificate()
		{
			key = scratch("peer.key");
			crt = scratch("peer.crt");

			/* Neither file is overwritten by the generator, so one left behind
			   by an earlier run is used as it stands rather than regenerated -
			   a 3072 bit key is not cheap. */
			if (!Samurai::IO::File(key).exists() || !Samurai::IO::File(crt).exists())
			{
				Samurai::IO::File(key).remove();
				Samurai::IO::File(crt).remove();
				Samurai::IO::Net::TlsFactory::generateSelfSignedCertificate(
					key.c_str(), crt.c_str(), QUICKDC_CERTIFICATE_NAME);
			}

			usable = Samurai::IO::File(key).exists() && Samurai::IO::File(crt).exists();
			if (usable)
				Samurai::IO::Net::TlsFactory::setKeys(key.c_str(), crt.c_str());
		}

		~Certificate()
		{
			/* Pointed at a pair that is not there, keys being replaceable but not
			   removable from outside the factory. */
			Samurai::IO::Net::TlsFactory::setKeys(scratch("none.key").c_str(),
				scratch("none.crt").c_str());
		}

		Certificate(const Certificate&) = delete;
		Certificate& operator=(const Certificate&) = delete;

		bool usable = false;

	private:
		std::string key;
		std::string crt;
};

/* A user for the session to be told to connect to. */
bool announcePeer(Session& s, const char* sid)
{
	s.say(std::string("BINF ") + sid + " NIpeer I4127.0.0.1 U41412 SS100 SL1");
	return true;
}

/*
 * A peer listener on the core, which is what makes this client active.
 *
 * The core the rest of this file uses is built without one - it is the
 * constructor meant for a test - and peerEndpoint() answers passive whenever
 * there is nothing listening, however the preference is set. So everything the
 * active side of a session does was unreachable from here, which is how a CTM
 * naming the wrong port went unnoticed.
 *
 * Bound on the loopback and on whatever port the system gives, so nothing here
 * depends on 1411 being free.
 */
/* Somewhere to point a socket that must not be answered. */
class Ignoring final : public Samurai::IO::Net::ServerSocketEventHandler,
                       public Samurai::IO::Net::SocketEventHandler
{
	public:
		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }
		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket>) override { }

		/* Everything a socket reports has a do-nothing default; only the two
		   above are demanded. */
};

/*
 * A port nothing is listening on, found by taking one and letting it go.
 *
 * DCServer binds whatever the configuration says, so the number has to be settled
 * before it is built for anything to be able to connect to it afterwards.
 */
uint16_t sparePort()
{
	Ignoring nobody;
	Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
	auto probe = Samurai::IO::Net::ServerSocket::create(&nobody, any);
	if (!probe || probe->getFD() == -1 || !probe->listen()) return 0;
	return probe->getLocalPort();
}

class ActiveCore
{
	public:
		ActiveCore()
		{
			QuickDC::Preferences* config = core->config;
			config->setGroup("Network");

			previous_active = config->getBool("Active mode");
			config->setBool("Active mode", true);
			config->setString("Bind", "127.0.0.1");

			bound = sparePort();
			config->setNumber("Port", bound);

			QuickDC::DCServer::Endpoint at;
			at.port = bound;
			at.bind = "127.0.0.1";
			server = new QuickDC::DCServer(*core, at);
			core->server = server;
		}

		uint16_t port() const { return bound; }

		~ActiveCore()
		{
			core->server = 0;
			delete server;

			QuickDC::Preferences* config = core->config;
			config->setGroup("Network");
			config->setBool("Active mode", previous_active);
		}

		ActiveCore(const ActiveCore&) = delete;
		ActiveCore& operator=(const ActiveCore&) = delete;

	private:
		QuickDC::DCServer* server = 0;
		uint16_t bound = 0;
		bool previous_active = false;
};

}

/* On by default: a CTM naming plain ADC is refused rather than dialled. */
EXO_TEST(hubsession_a_plain_connect_request_is_refused_when_encryption_is_required,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	setRequireEncryption(true);
	EXO_ASSERT(announcePeer(s, "BBBB"));

	std::string line;
	s.hub.drain();
	s.hub.received.clear();

	/* The peer says: dial me, in the clear, on this port. */
	s.say("DCTM BBBB AAAA ADC/1.0 1412 job1");

	/* Refused, and the peer is told why rather than left waiting. */
	/*
	 * nextLine() waits for a line, so a refusal that never came fails here - and
	 * the reason is asserted because a DSTA also goes back when an accepted
	 * request is dialled and the dial fails. Only this one names encryption.
	 */
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DSTA ") == 0);
	EXO_ASSERT(line.find("Encryption") != std::string::npos);
	EXO_ASSERT(line.find("job1") != std::string::npos);

	/* And the local side hears about it, since a transfer that never starts is
	   otherwise indistinguishable from a peer that is not answering. */
	EXO_ASSERT(!s.listener.errors.empty());
	return 1;
});

/*
 * The refusal names the peer and the protocol it asked for.
 *
 * This message repeats - once per request from every client that cannot do ADCS -
 * and without a name it says only that something was refused, which cannot tell one
 * peer trying over and over from half a hub each trying once. Neither can be worked
 * out from anywhere else afterwards: the request is gone by the time it is read.
 */
EXO_TEST(hubsession_an_unencrypted_refusal_names_the_peer_it_refused,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	setRequireEncryption(true);
	EXO_ASSERT(announcePeer(s, "BBBB"));

	s.hub.drain();
	s.hub.received.clear();

	s.say("DCTM BBBB AAAA ADC/1.0 1412 job1");

	bool named = false;
	for (const std::string& e : s.listener.errors)
		if (e.find("'peer'") != std::string::npos
			&& e.find(ADC_COMPLIANCE) != std::string::npos) named = true;

	if (!named)
	{
		const std::string why = s.listener.errors.empty()
			? std::string("nothing was reported at all")
			: std::string("reported \"") + s.listener.errors.front() + "\"";
		EXO_FAIL(why.c_str());
	}
	return 1;
});

/* And a peer the hub has said nothing about is still identified, by the session
   the request arrived from - the case where there is no nick to give. */
EXO_TEST(hubsession_a_refusal_falls_back_to_the_session_id,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	setRequireEncryption(true);
	EXO_ASSERT(announcePeer(s, "BBBB"));

	s.hub.drain();
	s.hub.received.clear();

	/* Announced with no NI at all, so the user record carries no nick. */
	s.say("BINF CCCC I4127.0.0.1 U41412 SS100 SL1");
	s.say("DCTM CCCC AAAA ADC/1.0 1412 job2");

	bool named = false;
	for (const std::string& e : s.listener.errors)
		if (e.find("CCCC") != std::string::npos) named = true;
	EXO_ASSERT(named);
	return 1;
});

/* With the requirement off, the same request is accepted. */
EXO_TEST(hubsession_a_plain_connect_request_is_accepted_when_encryption_is_not_required,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	setRequireEncryption(false);
	EXO_ASSERT(announcePeer(s, "BBBB"));

	std::string line;
	s.hub.drain();
	s.hub.received.clear();

	s.say("DCTM BBBB AAAA ADC/1.0 1412 job1");

	/*
	 * The request is accepted, so we dial - and nothing is listening on that
	 * port, so a DSTA does come back. What matters is which one: a refusal for
	 * being unencrypted would have been sent instead of dialling at all, so the
	 * assertion is on the reason rather than on the absence of a report.
	 */
	pump([&] { return false; }, 300);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("Encryption\\sis\\srequired") == std::string::npos);
	EXO_ASSERT(s.hub.received.find("Encryption is required") == std::string::npos);

	/* And the local side was told the dial failed, not that we refused it. */
	bool refusedForEncryption = false;
	for (const std::string& e : s.listener.errors)
		if (e.find("unencrypted") != std::string::npos) refusedForEncryption = true;
	EXO_ASSERT(!refusedForEncryption);
	return 1;
});

/*
 * An RCM asking for a plain connection is refused too. This is the direction we
 * would otherwise answer with a CTM of our own, naming what the peer asked for -
 * so it is the last point at which the requirement can be honoured.
 */
EXO_TEST(hubsession_a_plain_reverse_request_is_refused_when_encryption_is_required,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	setRequireEncryption(true);
	EXO_ASSERT(announcePeer(s, "BBBB"));

	std::string line;
	s.hub.drain();
	s.hub.received.clear();

	s.say("DRCM BBBB AAAA ADC/1.0 job2");

	/*
	 * nextLine() waits for a line, so a refusal that never came fails here - and
	 * the reason is asserted because a DSTA also goes back when an accepted
	 * request is dialled and the dial fails. Only this one names encryption.
	 */
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DSTA ") == 0);
	EXO_ASSERT(line.find("Encryption") != std::string::npos);
	EXO_ASSERT(line.find("job2") != std::string::npos);
	EXO_ASSERT(!s.listener.errors.empty());
	return 1;
});

/*
 * A protocol we do not speak at all is still refused whatever the setting says -
 * the requirement narrows what is acceptable and must not widen it.
 */
EXO_TEST(hubsession_an_unknown_protocol_is_refused_whatever_the_setting,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	setRequireEncryption(false);
	EXO_ASSERT(announcePeer(s, "BBBB"));

	std::string line;
	s.hub.drain();
	s.hub.received.clear();

	s.say("DCTM BBBB AAAA SOMETHING/9.9 1412 job3");

	/*
	 * Refused for not being a protocol we speak rather than for being plaintext,
	 * so this is the one refusal whose reason is not about encryption - and the
	 * setting is off above, which is what makes that distinction meaningful.
	 */
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DSTA ") == 0);
	EXO_ASSERT(line.find("Protocol") != std::string::npos);
	EXO_ASSERT(line.find("job3") != std::string::npos);
	return 1;
});

/*
 * A hub reached over ADCS requires its peer connections to be encrypted too, and
 * does not ask the preference: a client that took the trouble to reach the hub
 * encrypted has said what it wants, and the files a peer connection carries are the
 * same ones the session does. The setting is off below, which is what makes this
 * the hub's decision rather than a default being read.
 */
EXO_TEST(hubsession_an_adcs_hub_requires_encrypted_peer_connections,
{
	/*
	 * Asked of a session rather than driven over the wire, because a session that
	 * believes it is on an ADCS hub opens with a TLS handshake and the hub here is
	 * a plain socket - there is no logging in to one. What the rule decides is
	 * asserted on the plain hub in the cases either side of this; what is left to
	 * establish is that an ADCS hub does not consult the preference, and the
	 * preference is off below.
	 */
	class Probe : public ADC::HubSession
	{
		public:
			Probe(QuickDC::Hub* ui, QuickDC::HubListener* l,
			      Samurai::IO::Net::URL* url, bool secure)
				: ADC::HubSession(ui, l, url, secure) { }

			using ADC::HubSession::mustEncryptPeers;
	};

	setRequireEncryption(false);

	Listener listener;
	Samurai::IO::Net::URL url("adcs://127.0.0.1:1511");
	QuickDC::Hub ui(*core, &listener, &url);

	Probe encrypted(&ui, &listener, &url, true);
	EXO_ASSERT(encrypted.mustEncryptPeers());

	/* And a plain hub is the one that asks. */
	Probe plain(&ui, &listener, &url, false);
	EXO_ASSERT(!plain.mustEncryptPeers());

	setRequireEncryption(true);
	EXO_ASSERT(plain.mustEncryptPeers());
	return 1;
});

/*
 * And a plain ADC hub does not, which is the other half of the same rule: ADC/1.0
 * is what a client without ADCS has to offer, and a client that reached this hub in
 * the clear is not owed a stricter peer connection than the one it is using. The
 * setting is absent here rather than off, since the default is the claim.
 */
EXO_TEST(hubsession_a_plain_hub_accepts_plain_adc_when_nothing_says_otherwise,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	forgetEncryptionSetting();
	EXO_ASSERT(announcePeer(s, "BBBB"));

	s.hub.drain();
	s.hub.received.clear();

	s.say("DCTM BBBB AAAA ADC/1.0 1412 job1");

	/*
	 * Accepted, so we dial - and nothing is listening on that port, so a DSTA does
	 * come back. Which one it is, is the whole of the case: a refusal for being
	 * unencrypted would have gone out instead of dialling at all.
	 */
	pump([&] { return false; }, 300);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("Encryption\\sis\\srequired") == std::string::npos);

	bool refusedForEncryption = false;
	for (const std::string& e : s.listener.errors)
		if (e.find("unencrypted") != std::string::npos) refusedForEncryption = true;
	EXO_ASSERT(!refusedForEncryption);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The salt a hub asks for a password with                                   */
/* ------------------------------------------------------------------------- */

namespace {

/* Twenty-six base32 characters, the shortest that carries the 16 bytes now
   required; the pair differ so that two answers can be compared. */
const char* SALT_OK = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char* SALT_OK2 = "234567ABCDEFGHIJKLMNOPQRST";

bool warnedAbout(const Listener& listener, const char* what)
{
	for (const std::string& e : listener.errors)
		if (e.find(what) != std::string::npos) return true;
	return false;
}

/*
 * GPA belongs to the identify phase - after the SID and our own INF, before the
 * hub accepts it - so a case cannot get there through login().
 */
bool reachIdentify(Session& s)
{
	std::string line;
	if (!s.hub.nextLine(line)) return false;
	s.say("ISUP ADBASE");
	s.say("ISID AAAA");
	if (!s.hub.nextLine(line)) return false;
	return line.compare(0, 4, "BINF") == 0;
}

}

/*
 * The hub picks the salt and we hash the password with it, so everything that
 * makes the answer worth more than the password itself rests on the salt being
 * long and fresh - and the hub is exactly who cannot be trusted with that.
 *
 * A single base32 character decodes to no bytes at all, which makes the answer
 * base32(Tiger(password)): the same string every time, for ever, and replayable
 * by anyone who ever saw one.
 */
EXO_TEST(hubsession_a_salt_too_short_to_matter_is_not_answered,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(reachIdentify(s));

	s.hub.received.clear();
	s.say("IGPA A");

	/* Nothing is answered, and the user is told rather than left waiting. */
	pump([] { return false; }, 300);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("HPAS") == std::string::npos);
	EXO_ASSERT(warnedAbout(s.listener, "randomness"));
	return 1;
});

/* The floor itself is answered, so the case above is about the length. */
EXO_TEST(hubsession_a_salt_at_the_floor_is_answered,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(reachIdentify(s));

	s.say(std::string("IGPA ") + SALT_OK);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HPAS") == 0);
	return 1;
});

/* One character short of it is not. */
EXO_TEST(hubsession_a_salt_one_character_short_is_not_answered,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(reachIdentify(s));

	const std::string just_short(SALT_OK, strlen(SALT_OK) - 1);
	s.hub.received.clear();
	s.say("IGPA " + just_short);

	pump([] { return false; }, 300);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("HPAS") == std::string::npos);
	return 1;
});

/*
 * A salt this hub has used before is not answered again.
 *
 * Seeded rather than driven by two connections, because the exchange happens once
 * per connection: what is remembered is remembered against the hub, so a hub
 * offering last login's salt is the case, and this is that state.
 */
EXO_TEST(hubsession_a_salt_this_hub_used_before_is_not_answered,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(reachIdentify(s));

	/* What the previous login left behind, filed against this hub. */
	QuickDC::Preferences* store = core->session;
	store->setGroup(s.url->toString());
	store->setString(QUICKDC_ADC_SALT_SETTING, SALT_OK);

	s.hub.received.clear();
	s.say(std::string("IGPA ") + SALT_OK);

	pump([] { return false; }, 300);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("HPAS") == std::string::npos);
	EXO_ASSERT(warnedAbout(s.listener, "same randomness"));
	return 1;
});

/* A salt it has not used is answered, so the case above is about the reuse. */
EXO_TEST(hubsession_a_salt_this_hub_has_not_used_is_answered,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(reachIdentify(s));

	QuickDC::Preferences* store = core->session;
	store->setGroup(s.url->toString());
	store->setString(QUICKDC_ADC_SALT_SETTING, SALT_OK2);

	std::string line;
	s.say(std::string("IGPA ") + SALT_OK);
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HPAS") == 0);
	return 1;
});

/* And what was answered is remembered, or the next login would accept it again. */
EXO_TEST(hubsession_the_salt_answered_is_remembered_against_the_hub,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(reachIdentify(s));

	QuickDC::Preferences* store = core->session;
	store->setGroup(s.url->toString());
	store->remove(QUICKDC_ADC_SALT_SETTING);

	std::string line;
	s.say(std::string("IGPA ") + SALT_OK);
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HPAS") == 0);

	/* Written where the next connection to this hub will look. */
	store->setGroup(s.url->toString());
	EXO_ASSERT(store->getString(QUICKDC_ADC_SALT_SETTING, "") == std::string(SALT_OK));
	return 1;
});

/* A different salt gives a different answer, so the salt really is in the hash. */
EXO_TEST(hubsession_a_different_salt_gives_a_different_answer,
{
	std::string answers[2];

	for (int n = 0; n < 2; n++)
	{
		Session s;
		if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
		EXO_ASSERT(s.ready);
		EXO_ASSERT(reachIdentify(s));

		QuickDC::Preferences* store = core->session;
		store->setGroup(s.url->toString());
		store->remove(QUICKDC_ADC_SALT_SETTING);

		s.say(std::string("IGPA ") + (n == 0 ? SALT_OK : SALT_OK2));
		EXO_ASSERT(s.hub.nextLine(answers[n]));
		EXO_ASSERT(answers[n].compare(0, 4, "HPAS") == 0);
	}

	EXO_ASSERT(answers[0] != answers[1]);
	return 1;
});

/*
 * On a plain hub the answer crosses the clear, where it is not the password but
 * is enough to try every candidate against offline at whatever rate suits.
 * Reported rather than refused: what the user can change is the hub.
 */
EXO_TEST(hubsession_answering_over_an_unencrypted_hub_is_reported,
{
	Session s(false);
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(reachIdentify(s));

	std::string line;
	s.say(std::string("IGPA ") + SALT_OK);
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HPAS") == 0);

	/* Answered, and said. */
	EXO_ASSERT(warnedAbout(s.listener, "not encrypted"));
	return 1;
});


/* ------------------------------------------------------------------------- */
/* The connection request we send                                            */
/* ------------------------------------------------------------------------- */

/*
 * An active client asks the peer to come to us, and names the endpoint the INF
 * advertised rather than the configured port.
 *
 * The port used to be read straight from the configuration while the INF used
 * peerEndpoint(), so under UPnP - where the gateway may grant a port other than
 * the one asked for, which PortForward reports when it happens - a peer was sent
 * to a port with nothing behind it. That the two now come from one place is what
 * this pins; that peerEndpoint() answers with the granted port is pinned in
 * portforward.tcc, which is where a gateway can be faked.
 */
EXO_TEST(hubsession_an_active_client_asks_the_peer_to_connect_to_it,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	ActiveCore active;
	EXO_ASSERT(announcePeer(s, "BBBB"));

	const QuickDC::User* peer = s.session->getUser("BBBB");
	EXO_ASSERT_NOT_NULL(peer);

	s.hub.drain();
	s.hub.received.clear();

	s.session->sendConnectionRequest(peer, "job1");

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));

	/* A CTM, not an RCM: we can be connected to. */
	EXO_ASSERT(line.compare(0, 5, "DCTM ") == 0);
	EXO_ASSERT(line.find("job1") != std::string::npos);

	/* And the port is the one the INF advertised. */
	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	const std::string port = std::to_string(config->getNumber("Port"));
	EXO_ASSERT(line.find(" " + port + " ") != std::string::npos);
	return 1;
});

/*
 * And the port it names is the one the gateway granted, not the one configured.
 *
 * This is the case the bug lived in and the only one that can tell the two apart:
 * without a gateway they are the same number. The gateway here hands out 40001 for
 * a client that asked for something else, which is exactly what a real one does
 * when the port is already spoken for.
 */
EXO_TEST(hubsession_an_active_client_names_the_port_the_gateway_granted,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	FakeGateway gateway(IGD2);
	if (!gateway.ready()) EXO_SKIP("could not listen for the gateway");
	agreeable(gateway, 40001);

	ActiveCore active;

	/* Forwarded, and the granted port is not the one asked for. */
	Recorder events;
	QuickDC::PortForward forward(&events);
	forward.start(1411, gateway.location());
	EXO_ASSERT(pump([&] { return events.done(); }));
	EXO_ASSERT(events.forwarded == 1);
	EXO_ASSERT(events.port == 40001);

	QuickDC::PortForward* previous = core->portforward;
	core->portforward = &forward;

	EXO_ASSERT(announcePeer(s, "BBBB"));
	const QuickDC::User* peer = s.session->getUser("BBBB");
	EXO_ASSERT_NOT_NULL(peer);

	s.hub.drain();
	s.hub.received.clear();
	s.session->sendConnectionRequest(peer, "job4");

	std::string line;
	const bool got = s.hub.nextLine(line);

	/* Put the core back before asserting, since a failed assertion leaves. */
	core->portforward = previous;

	EXO_ASSERT(got);
	EXO_ASSERT(line.compare(0, 5, "DCTM ") == 0);
	EXO_ASSERT(line.find(" 40001 ") != std::string::npos);

	/* And not the configured one, which is what it used to name. */
	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	const std::string configured = std::to_string(config->getNumber("Port"));
	EXO_ASSERT(line.find(" " + configured + " ") == std::string::npos);
	return 1;
});

/* A passive client asks the other way round, and names no port at all - there is
   nothing to name. */
EXO_TEST(hubsession_a_passive_client_asks_the_peer_to_invite_it,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	EXO_ASSERT(announcePeer(s, "BBBB"));

	const QuickDC::User* peer = s.session->getUser("BBBB");
	EXO_ASSERT_NOT_NULL(peer);

	s.hub.drain();
	s.hub.received.clear();

	/* No listener on the core, so passive whatever the preference says. */
	s.session->sendConnectionRequest(peer, "job2");

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DRCM ") == 0);
	EXO_ASSERT(line.find("job2") != std::string::npos);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Withdrawing the claim that peers can reach us                             */
/* ------------------------------------------------------------------------- */

namespace {

/* Enough distinct peers reporting a failure to settle the question. */
void reportRefusals(QuickDC::Reachability* known)
{
	for (int n = 0; n < QUICKDC_REACHABILITY_REFUSALS; n++)
		known->connectionRefused("CID" + std::to_string(n));
}

/*
 * The INF this session sends when asked directly.
 *
 * Asked of the session rather than through the manager: this fixture's session is
 * not one the process-wide manager drives, so going through it would skip every
 * case here and read as coverage that is not there.
 *
 * The whole of it rather than the difference, since the cases below read several
 * fields out of the one line: an INF only says what has changed, so a case asking
 * about SU would otherwise find I4 missing and conclude the address had been
 * withdrawn, when all that happened was that it stayed the same.
 */
bool announcedSupport(Session& s, std::string& out)
{
	s.hub.drain();
	s.hub.received.clear();

	s.session->forgetAnnouncedInfo();
	s.session->sendClientInfo();

	std::string line;
	if (!s.hub.nextLine(line)) return false;
	if (line.compare(0, 4, "BINF") != 0) return false;

	out = line;
	return true;
}

}

/*
 * Either spelling of the protocol is recognised as encryption.
 *
 * A peer naming ADCS/1.0 - the spelling ADC-EXT specifies - used to be turned away
 * for naming a protocol we do not speak, because the comparison was against
 * ADCS/0.10 alone. With encryption required by default that refusal was the whole
 * transfer, in the one case where both ends wanted exactly the same thing.
 *
 * Asserted on the reason rather than on a connection, because this fixture has no
 * certificate: what is being distinguished is "we understood you and cannot hold
 * up our end" from "we have no idea what you are asking for".
 */
EXO_TEST(hubsession_either_spelling_of_the_encrypted_protocol_is_recognised,
{
	for (const char* asked : { ADC_TLS_PROTO, ADC_TLS_PROTO_DRAFT })
	{
		Session s;
		if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
		EXO_ASSERT(s.ready);
		EXO_ASSERT(s.login());

		EXO_ASSERT(announcePeer(s, "BBBB"));

		s.hub.drain();
		s.hub.received.clear();

		/* An RCM: the peer cannot be connected to and asks us to dial it. */
		s.say(std::string("DRCM BBBB AAAA ") + asked + " job1");

		std::string line;
		EXO_ASSERT(s.hub.nextLine(line));
		EXO_ASSERT(line.compare(0, 5, "DSTA ") == 0);
		EXO_ASSERT(line.find("job1") != std::string::npos);

		/* Understood as TLS - the objection is our own certificate. */
		if (line.find("certificate") == std::string::npos)
		{
			const std::string why = std::string("asked for ") + asked
				+ " and was refused with \"" + line + "\"";
			EXO_FAIL(why.c_str());
		}
		EXO_ASSERT(line.find("Protocol\\snot\\ssupported") == std::string::npos);
	}
	return 1;
});

/* And a protocol that is neither is still not understood, which is the boundary. */
EXO_TEST(hubsession_a_protocol_that_is_not_adc_at_all_is_still_refused,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(announcePeer(s, "BBBB"));

	s.hub.drain();
	s.hub.received.clear();

	/* Close enough to be caught by a substring test, and not a protocol. */
	s.say("DRCM BBBB AAAA ADCS/9.9 job1");

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DSTA ") == 0);
	EXO_ASSERT(line.find("Protocol\\snot\\ssupported") != std::string::npos);
	return 1;
});

/*
 * Encryption is announced in both spellings.
 *
 * ADCS is what ADC-EXT names: "Clients supporting encrypted connections must
 * indicate this in the INF SU field with ADCS." ADC0 is what the flag was called
 * before the extension was standardised, and a fourcc in SU is matched whole - so
 * to the DC++ family, which looks for ADC0 and for nothing else, a client claiming
 * ADCS alone is a client with no encryption at all, and it arranges a plaintext
 * transfer or none.
 */
EXO_TEST(hubsession_encryption_is_announced_in_both_spellings,
{
	Certificate cert;
	if (!cert.usable) EXO_SKIP("could not generate a certificate");

	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(claims(inf, ADC_EXT_ADCS));
	EXO_ASSERT(claims(inf, ADC_EXT_ADCS_DRAFT));
	return 1;
});

/*
 * And neither spelling without a certificate, because this is the flag a peer reads
 * to decide whether to ask for ADCS: claiming it with nothing to present sends the
 * peer at a handshake that cannot complete, which costs it the transfer it would
 * otherwise have had in the clear.
 */
EXO_TEST(hubsession_encryption_is_not_announced_without_a_certificate,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(!claims(inf, ADC_EXT_ADCS));
	EXO_ASSERT(!claims(inf, ADC_EXT_ADCS_DRAFT));

	/* The rest of the field is still there, so this is a flag withheld rather
	   than an INF that says nothing. */
	EXO_ASSERT(claims(inf, ADC_EXT_RICHTEXT));
	return 1;
});

/*
 * And the draft spelling is still understood when a peer names it. ADC-EXT settled
 * on ADCS/1.0, which is what goes out; ADCS/0.10 is the number the extension had
 * while it was a draft, and the DC++ family still sends it. Refusing it would
 * refuse the transfer.
 */
EXO_TEST(hubsession_both_spellings_of_the_encrypted_protocol_are_understood,
{
	EXO_ASSERT(adc_is_tls_protocol(ADC_TLS_PROTO));
	EXO_ASSERT(adc_is_tls_protocol(ADC_TLS_PROTO_DRAFT));
	EXO_ASSERT(adc_is_tls_protocol("ADCS/0.10"));
	EXO_ASSERT(adc_is_tls_protocol("ADCS/1.0"));

	/* And nothing else is, least of all plain ADC. */
	EXO_ASSERT(!adc_is_tls_protocol(ADC_COMPLIANCE));
	EXO_ASSERT(!adc_is_tls_protocol("ADCS"));
	EXO_ASSERT(!adc_is_tls_protocol("ADCS/2.0"));
	EXO_ASSERT(!adc_is_tls_protocol(""));
	return 1;
});

/*
 * An active client says TCP4 - a claim other clients act on by telling peers to
 * connect here.
 */
EXO_TEST(hubsession_an_active_client_claims_tcp4,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	core->reachability->forget();

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(claims(inf, "TCP4"));
	EXO_ASSERT(claims(inf, "UDP4"));
	return 1;
});

/*
 * And stops once enough peers have said they could not get in. Withdrawing it is
 * not only honesty: NATT engages only when neither side claims TCP4, so a false
 * claim suppresses the fallback that would have worked.
 */
EXO_TEST(hubsession_the_tcp4_claim_is_withdrawn_once_peers_report_failing,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	QuickDC::Reachability* known = core->reachability;
	known->forget();
	reportRefusals(known);

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(!claims(inf, "TCP4"));

	/*
	 * UDP4 stands. Nothing can disprove it - a peer that cannot reach the port has
	 * no datagram it could send to say so - so it is not withdrawn on the strength
	 * of what the TCP reports said.
	 */
	EXO_ASSERT(claims(inf, "UDP4"));

	/* Still an endpoint to publish: passive for connections, not absent. */
	EXO_ASSERT(hasField(inf, "I4"));
	return 1;
});

/* A connection that arrived outranks the reports, so the claim stands. */
EXO_TEST(hubsession_a_connection_that_arrived_keeps_the_claim,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	QuickDC::Reachability* known = core->reachability;
	known->forget();
	reportRefusals(known);
	known->inboundConnection();

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(claims(inf, "TCP4"));
	return 1;
});

/* And a user who knows their own network can say so. */
EXO_TEST(hubsession_forcing_active_mode_overrules_the_reports,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	QuickDC::Reachability* known = core->reachability;
	known->forget();
	reportRefusals(known);

	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->setBool("Force active mode", true);

	std::string inf;
	const bool announced = announcedSupport(s, inf);

	/* Put back before asserting, since a failed assertion leaves. */
	config->setGroup("Network");
	config->setBool("Force active mode", false);

	EXO_ASSERT(announced);
	EXO_ASSERT(claims(inf, "TCP4"));
	return 1;
});

/*
 * A connection from this machine is not evidence of anything.
 *
 * The peer port being open was never the question - the question is whether
 * anything outside can reach it, and the loopback cannot answer that. Counting it
 * would make every client claim to be reachable the moment anything local
 * connected, which on a machine also running a hub is immediately.
 */
EXO_TEST(hubsession_a_loopback_connection_is_not_evidence_of_reachability,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	ActiveCore active;
	if (!active.port()) EXO_SKIP("could not reserve a port for the peer listener");

	QuickDC::Reachability* known = core->reachability;
	known->forget();
	EXO_ASSERT(known->tcp() == QuickDC::Reachability::State::Unknown);

	/* Straight at our own peer port, over the loopback. */
	Ignoring nobody;
	Samurai::IO::Net::InetAddress local("127.0.0.1");
	auto client = Samurai::IO::Net::Socket::create(&nobody, local, active.port());
	EXO_ASSERT(client != 0);
	client->connect();

	/* Accepted - the listener is real - and still not evidence. */
	pump([] { return false; }, 400);
	EXO_ASSERT(known->tcp() == QuickDC::Reachability::State::Unknown);

	client.reset();
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The address, published either way                                         */
/* ------------------------------------------------------------------------- */

/*
 * A passive client publishes its address too.
 *
 * Sending it only when active reads as the private thing to do, and it is why this
 * was that way - but NAT traversal cannot work without it. Two passive clients
 * reach each other by dialling at once, and neither can dial an address the hub
 * never passed on.
 */
EXO_TEST(hubsession_a_passive_client_publishes_its_address,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(hasField(inf, "I4"));
	return 1;
});

/*
 * And publishes nothing that would have a peer connect to it: no port, and neither
 * TCP4 nor UDP4, so it still reads as somebody to invite rather than somebody to
 * dial.
 */
EXO_TEST(hubsession_a_passive_client_publishes_no_port_and_claims_nothing,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));
	EXO_ASSERT(!hasField(inf, "U4"));
	EXO_ASSERT(!claims(inf, "TCP4"));
	EXO_ASSERT(!claims(inf, "UDP4"));
	return 1;
});

/*
 * A port that was published and is no longer open is withdrawn, rather than left
 * behind for the hub to keep handing out.
 *
 * Withdrawing it means sending the field with nothing after it, which is how ADC
 * says to drop one. Leaving it out of the update instead says only that it has
 * not changed, and every peer goes on being told to connect to a port that is
 * shut - which is also what suppresses the NAT traversal that would have worked.
 */
EXO_TEST(hubsession_a_port_that_closed_is_withdrawn,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	{
		ActiveCore active;
		EXO_ASSERT(s.login());

		std::string inf;
		EXO_ASSERT(announcedSupport(s, inf));
		EXO_ASSERT(!infField(inf, "U4").empty());
	}

	s.hub.drain();
	s.hub.received.clear();

	s.session->sendClientInfo();
	pump([] { return false; }, 60);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "BINF") == 0);

	EXO_ASSERT(hasField(line, "U4"));
	EXO_ASSERT(infField(line, "U4").empty());
	return 1;
});

/*
 * With nothing known, the address published is the one ADC reads as a question: fill
 * in what you see. That is how a client behind a NAT nothing has told it about finds
 * out where it is.
 */
EXO_TEST(hubsession_a_client_with_no_known_address_asks_for_one,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->setString("Address", "0.0.0.0");

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(infField(inf, "I4") == "0.0.0.0");
	return 1;
});

/* And once a hub has said where it sees us, that is what goes out - even passive,
   where peerEndpoint() has no address to offer because there is nowhere to connect. */
EXO_TEST(hubsession_a_passive_client_publishes_the_address_the_hub_reported,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	/* The hub refusing what we claimed and saying what it sees instead. */
	s.say("ISTA 146 Invalid\\sIP IP198.51.100.9");
	pump([] { return false; }, 200);

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(infField(inf, "I4") == "198.51.100.9");
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What the hub says about a connection it made itself                       */
/* ------------------------------------------------------------------------- */

/*
 * A hub that checked the port we published and got nothing is worth more than a
 * peer's report of the same thing, and is acted on at once rather than after three
 * of them. A peer's DSTA is hearsay: it comes through the hub quoting a token
 * anybody on the hub saw. This comes from the party we authenticated to, about an
 * attempt it made itself.
 *
 * Told apart by two things and no new wire format: ISTA is the hub speaking, and no
 * 'TO' means it is not the answer to any transfer we asked for.
 */
EXO_TEST(hubsession_a_hub_that_could_not_connect_withdraws_the_claim,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	QuickDC::Reachability* known = core->reachability;
	known->forget();

	/* Claimed to begin with, so what follows is a change and not the status quo. */
	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));
	EXO_ASSERT(claims(inf, "TCP4"));

	s.say("ISTA 142 Nothing\\sanswered\\son\\sport\\s1411 PRADC/1.0");

	EXO_ASSERT(announcedSupport(s, inf));
	EXO_ASSERT(!claims(inf, "TCP4"));

	/* UDP4 stands: a failed TCP connection says nothing about a datagram port. */
	EXO_ASSERT(claims(inf, "UDP4"));

	/* And there is still an address to publish, which NATT needs. */
	EXO_ASSERT(hasField(inf, "I4"));
	return 1;
});

/*
 * Said again without being asked, so peers stop being told to connect to a port
 * nothing answers on - and so NATT is no longer suppressed by a claim that was false.
 *
 * Asserted on the request rather than on the wire. The update is coalesced through
 * the hub manager, which is what stops a client on five hubs from announcing five
 * times over, and the manager does not drive this fixture's session - so a case
 * waiting for the INF would be waiting for something that was never going to arrive
 * here, and would pass or fail for reasons of its own.
 */
EXO_TEST(hubsession_a_failed_check_asks_for_the_claim_to_be_said_again,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	core->reachability->forget();

	QuickDC::HubManager* hubs = core->hubs;

	/* Long enough that nothing scheduled goes out while the case is running, so
	   what is observed is this request and not the tail of an earlier one. */
	hubs->setClientInfoUpdateDelay(std::chrono::seconds(30));
	hubs->sendClientInfoToAllHubs();
	const bool quiet = !hubs->isClientInfoUpdatePending();

	s.say("ISTA 142 Nothing\\sanswered PRADC/1.0");

	const bool asked = hubs->isClientInfoUpdatePending();

	/* Put back before anything is asserted, since a failed assertion leaves - and
	   flushed, so no case after this one starts with an update scheduled. */
	hubs->setClientInfoUpdateDelay(
		std::chrono::seconds(QUICKDC_HUB_INFO_UPDATE_DELAY_SECONDS));
	hubs->sendClientInfoToAllHubs();

	EXO_ASSERT(quiet);
	EXO_ASSERT(asked);
	return 1;
});

/*
 * A status carrying a 'TO' is a peer's report relayed through, whatever context it
 * arrives in - one peer's account of its own failure, which on its own decides
 * nothing. Otherwise a single peer could argue us into being passive by borrowing
 * the hub's voice.
 */
EXO_TEST(hubsession_a_status_naming_a_transfer_is_not_the_hubs_own_check,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	core->reachability->forget();

	s.say("ISTA 142 Could\\snot\\sconnect TOsometoken PRADC/1.0");

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));
	EXO_ASSERT(claims(inf, "TCP4"));
	return 1;
});

/* Nor is a status from another client, whatever it says. */
EXO_TEST(hubsession_a_status_from_a_peer_is_not_the_hubs_own_check,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	core->reachability->forget();

	s.say("DSTA BBBB AAAA 142 Could\\snot\\sconnect PRADC/1.0");

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));
	EXO_ASSERT(claims(inf, "TCP4"));
	return 1;
});

/* And a user who knows their own network still has the last word. */
EXO_TEST(hubsession_forcing_active_mode_overrules_the_hubs_check,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	core->reachability->forget();

	s.say("ISTA 142 Nothing\\sanswered PRADC/1.0");

	QuickDC::Preferences* config = core->config;
	config->setGroup("Network");
	config->setBool("Force active mode", true);

	std::string inf;
	const bool announced = announcedSupport(s, inf);

	config->setGroup("Network");
	config->setBool("Force active mode", false);

	EXO_ASSERT(announced);
	EXO_ASSERT(claims(inf, "TCP4"));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Reaching a peer that cannot be connected to either                        */
/* ------------------------------------------------------------------------- */

namespace {

/* A peer on the hub announcing what it can do. */
bool announcePeerSupporting(Session& s, const char* sid, const char* su)
{
	s.say(std::string("BINF ") + sid + " NIpeer I4198.51.100.9 SS100 SL1 SU" + su);
	return true;
}

}

/*
 * Two clients that can neither of them be connected to used to end here with
 * nothing to offer each other: the reverse request came in, we could not answer it
 * with a CTM, and the exchange stopped at "both clients are passive".
 *
 * With NATT there is something to offer. Both can dial, and two connections started
 * at the same moment cross in the middle, so we name the port ours will come from.
 */
EXO_TEST(hubsession_two_passive_clients_arrange_to_traverse,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	EXO_ASSERT(announcePeerSupporting(s, "BBBB", "ADC0,NATT"));

	setRequireEncryption(false);
	s.hub.drain();
	s.hub.received.clear();

	/* The peer asks us to invite it, and we cannot. */
	s.say("DRCM BBBB AAAA ADC/1.0 job1");

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DNAT ") == 0);
	EXO_ASSERT(line.find("job1") != std::string::npos);
	return 1;
});

/* A peer that cannot traverse still gets the old answer, because there is still
   nothing to be done for it. */
EXO_TEST(hubsession_a_passive_peer_without_traversal_is_told_so,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	EXO_ASSERT(announcePeerSupporting(s, "BBBB", "ADC0"));

	setRequireEncryption(false);
	s.hub.drain();
	s.hub.received.clear();

	s.say("DRCM BBBB AAAA ADC/1.0 job2");

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DSTA ") == 0);
	EXO_ASSERT(line.find("passive") != std::string::npos);
	return 1;
});

/*
 * A peer that named the port it will dial from is answered with ours, and answered
 * before we dial: the two connections have to cross, so the peer must be starting
 * at the same time rather than after us.
 */
EXO_TEST(hubsession_a_traversal_request_is_answered_with_our_own_port,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	EXO_ASSERT(announcePeerSupporting(s, "BBBB", "ADC0,NATT"));

	setRequireEncryption(false);
	s.hub.drain();
	s.hub.received.clear();

	s.say("DNAT BBBB AAAA ADC/1.0 2000 job3");

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DRNT ") == 0);
	EXO_ASSERT(line.find("job3") != std::string::npos);

	/*
	 * The port named is this hub connection's own. Read from the hub's side of it
	 * rather than asked of the session, which is both independent of the code under
	 * test and exactly what the specification means: the outbound port to the hub,
	 * which is the port the hub sees the connection arrive from.
	 */
	EXO_ASSERT(s.hub.peer != 0);
	const uint16_t mine = s.hub.peer->getPort();
	EXO_ASSERT(mine != 0);
	EXO_ASSERT(line.find(" " + std::to_string(mine) + " ") != std::string::npos);
	return 1;
});

/* An answer to our own request is acted on and not answered again, or the two
   would talk past each other for ever. */
EXO_TEST(hubsession_a_traversal_answer_is_not_answered_again,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	EXO_ASSERT(announcePeerSupporting(s, "BBBB", "ADC0,NATT"));

	setRequireEncryption(false);
	s.hub.drain();
	s.hub.received.clear();

	s.say("DRNT BBBB AAAA ADC/1.0 2000 job4");

	pump([] { return false; }, 300);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("DRNT") == std::string::npos);
	EXO_ASSERT(s.hub.received.find("DNAT") == std::string::npos);
	return 1;
});

/* And we say we can do it, so a peer knows to offer - in both spellings, for the
   same reason encryption is claimed in both: NATT is what ADC-EXT names and NAT0
   is what the clients that implement it look for. */
EXO_TEST(hubsession_a_client_announces_that_it_can_traverse,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));
	EXO_ASSERT(claims(inf, ADC_EXT_NATT));
	EXO_ASSERT(claims(inf, ADC_EXT_NATT_DRAFT));
	return 1;
});

/* Read the same way round: a peer that claims only the draft flag is one to
   traverse to, that being the flag the clients doing this in practice send. */
EXO_TEST(hubsession_a_peer_claiming_only_the_draft_traversal_flag_is_traversed_to,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	EXO_ASSERT(announcePeerSupporting(s, "BBBB", "ADC0,NAT0"));

	setRequireEncryption(false);
	s.hub.drain();
	s.hub.received.clear();

	s.say("DRCM BBBB AAAA ADC/1.0 job1");

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DNAT ") == 0);
	EXO_ASSERT(line.find("job1") != std::string::npos);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Which protocol a peer connection is arranged in                           */
/* ------------------------------------------------------------------------- */

/*
 * A CTM answering an RCM names what the RCM named, spelling and all.
 *
 * The peer dials on the strength of that field and compares it against the
 * protocols it knows, so a client that asked for ADCS/0.10 - which is what the DC++
 * family asks for - and is answered with ADCS/1.0 reads the answer as a protocol it
 * does not speak and refuses the connection it asked for itself. Both spellings
 * mean the same TLS, and this end was invited rather than asked.
 */
EXO_TEST(hubsession_a_reverse_request_is_answered_in_the_spelling_it_asked_for,
{
	for (const char* asked : { ADC_TLS_PROTO, ADC_TLS_PROTO_DRAFT })
	{
		Certificate cert;
		if (!cert.usable) EXO_SKIP("could not generate a certificate");

		Session s;
		if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
		EXO_ASSERT(s.ready);
		ActiveCore active;
		EXO_ASSERT(s.login());
		EXO_ASSERT(announcePeerSupporting(s, "BBBB", "ADCS,ADC0"));

		s.hub.drain();
		s.hub.received.clear();

		s.say(std::string("DRCM BBBB AAAA ") + asked + " job1");

		std::string line;
		EXO_ASSERT(s.hub.nextLine(line));

		if (line.compare(0, 5, "DCTM ") != 0
			|| line.find(std::string(" ") + asked + " ") == std::string::npos)
		{
			const std::string why = std::string("asked for ") + asked
				+ " and was answered with \"" + line + "\"";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/*
 * And a request we make ourselves names what the peer said it speaks, the same rule
 * read the other way round: SU is where a client says which spelling it answers to,
 * and it is the only thing that says so before the connection exists.
 */
EXO_TEST(hubsession_a_connect_request_names_what_the_peer_announced,
{
	struct Case { const char* su; const char* expected; };
	const Case cases[] = {
		{ "ADCS,ADC0,TCP4", ADC_TLS_PROTO },
		{ "ADCS,TCP4",      ADC_TLS_PROTO },
		{ "ADC0,TCP4",      ADC_TLS_PROTO_DRAFT },
		{ "TCP4",           ADC_COMPLIANCE },
	};

	for (const Case& c : cases)
	{
		Certificate cert;
		if (!cert.usable) EXO_SKIP("could not generate a certificate");

		Session s;
		if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
		EXO_ASSERT(s.ready);
		ActiveCore active;
		EXO_ASSERT(s.login());

		/* Off, so that a peer announcing nothing is asked for what it can do
		   rather than refused - which is the last case. */
		setRequireEncryption(false);
		EXO_ASSERT(announcePeerSupporting(s, "BBBB", c.su));

		s.hub.drain();
		s.hub.received.clear();

		const QuickDC::User* peer = s.session->getUserManager()->getUser("BBBB");
		EXO_ASSERT(peer != 0);
		s.session->sendConnectionRequest(peer, "job1");
		pump([] { return false; }, 60);

		std::string line;
		EXO_ASSERT(s.hub.nextLine(line));

		if (line.compare(0, 5, "DCTM ") != 0
			|| line.find(std::string(" ") + c.expected + " ") == std::string::npos)
		{
			const std::string why = std::string("a peer announcing ") + c.su
				+ " was sent \"" + line + "\"";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/*
 * The same when it is the peer that will dial: an RCM carries a protocol too, and a
 * passive client needs no certificate of its own to name encryption - it is the end
 * that connects out, and the certificate belongs to whoever answers.
 */
EXO_TEST(hubsession_a_reverse_request_names_what_the_peer_announced,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	setRequireEncryption(false);
	EXO_ASSERT(announcePeerSupporting(s, "BBBB", "ADC0,TCP4"));

	s.hub.drain();
	s.hub.received.clear();

	const QuickDC::User* peer = s.session->getUserManager()->getUser("BBBB");
	EXO_ASSERT(peer != 0);
	s.session->sendConnectionRequest(peer, "job1");
	pump([] { return false; }, 60);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DRCM ") == 0);
	EXO_ASSERT(line.find(std::string(" ") + ADC_TLS_PROTO_DRAFT + " ") != std::string::npos);
	return 1;
});

/*
 * A peer that announced no encryption at all is still asked for it where the hub
 * requires it, in the spelling the specification gives. SU is a claim a client has
 * to remember to make, and treating silence as "cannot" would turn every
 * under-announcing peer into a transfer refused before it was attempted.
 */
EXO_TEST(hubsession_a_silent_peer_is_still_asked_for_encryption_where_it_is_required,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	setRequireEncryption(true);
	EXO_ASSERT(announcePeer(s, "BBBB"));

	s.hub.drain();
	s.hub.received.clear();

	const QuickDC::User* peer = s.session->getUserManager()->getUser("BBBB");
	EXO_ASSERT(peer != 0);
	s.session->sendConnectionRequest(peer, "job1");
	pump([] { return false; }, 60);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 5, "DRCM ") == 0);
	EXO_ASSERT(line.find(std::string(" ") + ADC_TLS_PROTO + " ") != std::string::npos);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* A datagram as evidence                                                    */
/* ------------------------------------------------------------------------- */

namespace {

/*
 * A search result as it arrives on the wire, from wherever it says.
 *
 * Built rather than sent, because the one case that matters cannot be produced on a
 * test machine: evidence requires a source that is not this machine, and everything
 * a loopback test can send comes from this machine. The decision is what is under
 * test, so the decision is what is called.
 */
bool arrives(const char* from, const char* token)
{
	const std::string line = std::string("URES AAAA FNfile.txt SI100 SL1 TO") + token;

	ADC::Command cmd;
	if (!ADC::Parser::parse(line.c_str(), line.size(), cmd)) return false;

	Samurai::IO::Net::InetAddress address(from);
	Samurai::IO::Net::InetSocketAddress source(address, 1412);

	Samurai::IO::Net::DatagramPacket packet;
	packet.setAddress(&source);

	ADC::DatagramHandler::noteReachable(*core, &packet, &cmd);
	return true;
}

}

/*
 * A datagram answering a search of ours, from somewhere that is not this machine, is
 * the only thing that shows the peer port can be reached over UDP - no peer that
 * fails to reach it has any way of saying so.
 */
EXO_TEST(hubsession_a_datagram_answering_our_search_shows_udp_works,
{
	QuickDC::Reachability* known = core->reachability;
	QuickDC::SearchTokens* tokens = core->searchTokens;
	known->forget();
	tokens->forget();

	const std::string token = tokens->issue();
	EXO_ASSERT(!token.empty());
	EXO_ASSERT(known->udp() == QuickDC::Reachability::State::Unknown);

	EXO_ASSERT(arrives("198.51.100.9", token.c_str()));

	EXO_ASSERT(known->udp() == QuickDC::Reachability::State::Reachable);

	/* And says nothing about TCP, which is a separate mapping. */
	EXO_ASSERT(known->tcp() == QuickDC::Reachability::State::Unknown);
	return 1;
});

/*
 * One answering no search of ours is not evidence. A source address is whatever its
 * sender wrote, so an unsolicited datagram shows nothing at all - anyone could send
 * one claiming to be anywhere.
 */
EXO_TEST(hubsession_a_datagram_answering_nothing_is_not_evidence,
{
	QuickDC::Reachability* known = core->reachability;
	QuickDC::SearchTokens* tokens = core->searchTokens;
	known->forget();
	tokens->forget();

	tokens->issue();

	EXO_ASSERT(arrives("198.51.100.9", "NOTOURSATALL"));

	EXO_ASSERT(known->udp() == QuickDC::Reachability::State::Unknown);
	return 1;
});

/*
 * Nor is one from this machine, for the same reason a loopback connection is not:
 * it shows the socket is open, which was never the question.
 */
EXO_TEST(hubsession_a_datagram_from_this_machine_is_not_evidence,
{
	QuickDC::Reachability* known = core->reachability;
	QuickDC::SearchTokens* tokens = core->searchTokens;
	known->forget();
	tokens->forget();

	const std::string token = tokens->issue();
	EXO_ASSERT(!token.empty());

	EXO_ASSERT(arrives("127.0.0.1", token.c_str()));

	EXO_ASSERT(known->udp() == QuickDC::Reachability::State::Unknown);
	return 1;
});

/* And a search that went out is one whose answer can be recognised, which is what
   ties the two halves together. */
EXO_TEST(hubsession_a_search_that_went_out_is_remembered,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	QuickDC::SearchTokens* tokens = core->searchTokens;
	tokens->forget();

	s.hub.drain();
	s.hub.received.clear();

	QuickDC::Share::SearchRequest request(QuickDC::Share::SearchRequest::SizeAny,
		0, "AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A", 0, 0);
	core->hubs->searchAllHubs(&request);

	/* Whatever token it went out with is one an answer can be checked against. */
	const char* went = request.getToken();
	EXO_ASSERT_NOT_NULL(went);
	EXO_ASSERT(tokens->issued(went));
	return 1;
});

/*
 * One file this client offers by hash, and somewhere to write the file lists.
 *
 * Attached rather than shared, because attaching hashes exactly one named file
 * where a share would have to be scanned. The manager is replaced for the length
 * of the case: the fixture's own is pointed at the account's directory, and
 * hashing ends in a file list being written there.
 */
class Offered
{
	public:
		std::string tth;
		bool ready = false;

		Offered()
			: file(scratch("offered.bin")), lists(scratch("lists")),
			  cache(scratch("offered.cache"))
		{
			Samurai::IO::File::mkdir(lists.c_str());

			FILE* fh = fopen(file.c_str(), "wb");
			if (!fh) return;
			const std::string bytes(4096, 'q');
			const bool written = fwrite(bytes.data(), 1, bytes.size(), fh) == bytes.size();
			fclose(fh);
			if (!written) return;

		
			hashes = new QuickDC::Hash::Manager(cache.c_str());
			replaced = core->shares;
			mine = new QuickDC::Share::Manager(core->config, hashes, lists.c_str());
			core->shares = mine;

			if (!mine->attach(file.c_str(), &listener,
					QuickDC::Share::Manager::Persistence::Session,
					QuickDC::Share::Manager::SlotCost::Free))
				return;

			/* Hashed on the worker; nothing is known until this thread collects it. */
			const auto deadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(30);

			while (hashes->process() || hashes->isBusy())
			{
				if (std::chrono::steady_clock::now() > deadline) return;
				usleep(1000);
			}

			tth = listener.tth;
			ready = !tth.empty();
		}

		~Offered()
		{
			if (replaced)
			{
				core->shares = replaced;
				delete mine;
			}

			delete hashes;

			Samurai::IO::File(file).remove();
			Samurai::IO::File(cache).remove();
			Samurai::IO::File(cache + ".new").remove();
			Samurai::IO::File(lists + "/files.xml").remove();
			Samurai::IO::File(lists + "/files.xml.bz2").remove();
			Samurai::IO::File::rmdir(lists.c_str());
		}

		Offered(const Offered&) = delete;
		Offered& operator=(const Offered&) = delete;

	private:
		class Told : public QuickDC::Share::AttachmentListener
		{
			public:
				void EventAttachmentReady(const char*, const char* root,
				                          uint64_t) override
				{
					tth = root ? root : "";
				}

				void EventAttachmentFailed(const char*, const char*) override { }

				std::string tth;
		};

		std::string file;
		std::string lists;
		std::string cache;
		Told listener;
		QuickDC::Hash::Manager* hashes = 0;
		QuickDC::Share::Manager* mine = 0;
		QuickDC::Share::Manager* replaced = 0;
};

/*
 * A stranger's search for something we offer is answered, and our own is not.
 *
 * Both halves, because only the pair says anything: that the file is findable at
 * all, and that the very same search carrying our own session as its sender leaves
 * this client silent.
 */
EXO_TEST(hubsession_our_own_search_is_not_answered,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	Offered offered;
	if (!offered.ready) EXO_SKIP("could not hash a file to offer");

	/* Somebody else on the hub, who has no address, so a reply goes through it. */
	s.say("BINF BBBB NIalice");

	s.hub.drain();
	s.hub.received.clear();

	s.say(std::string("BSCH BBBB TR") + offered.tth + " TOstranger");
	pump([&] {
		s.hub.drain();
		return s.hub.received.find("DRES") != std::string::npos;
	}, 500);

	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("DRES") != std::string::npos);
	EXO_ASSERT(s.hub.received.find(offered.tth) != std::string::npos);

	s.hub.received.clear();

	/* The same search, said to come from us - which is what an echo looks like. */
	s.say(std::string("BSCH AAAA TR") + offered.tth + " TOourselves");
	pump([&] {
		s.hub.drain();
		return s.hub.received.find("DRES") != std::string::npos;
	}, 500);

	s.hub.drain();
	EXO_ASSERT(s.hub.received.find("DRES") == std::string::npos);
	return 1;
});

/*
 * A logged-in hub the manager can see, which is what a fan-out walks.
 *
 * Driven through Hub::connect() rather than by building the session directly, because
 * the manager reads each hub's session and only connect() sets one - see the
 * client-info cases above, which need the same thing.
 */
class ManagedHub
{
	public:
		FakeHub fake;
		Listener listener;
		std::unique_ptr<Samurai::IO::Net::URL> url;
		std::unique_ptr<QuickDC::Hub> ui;
		bool ready = false;

		ManagedHub()
		{
			if (!fake.listen()) return;

			const std::string address =
				"adc://127.0.0.1:" + std::to_string(fake.port());
			url = std::make_unique<Samurai::IO::Net::URL>(address);
			ui = std::make_unique<QuickDC::Hub>(*core, &listener, url.get());

			ui->connect();
			if (!fake.accepted()) return;

			std::string line;
			if (!fake.nextLine(line)) return;

			fake.send("ISUP ADBASE ADTIGR");
			pump([] { return false; }, 60);
			fake.send("ISID AAAA");
			pump([] { return false; }, 60);

			/* The INF echoed back is what being logged in means. */
			if (fake.nextLine(line) && line.compare(0, 4, "BINF") == 0)
			{
				fake.send(line);
				pump([] { return false; }, 60);
			}

			fake.drain();
			fake.received.clear();

			ready = true;
		}

		ManagedHub(const ManagedHub&) = delete;
		ManagedHub& operator=(const ManagedHub&) = delete;

		/** Whether anything carrying 'what' has reached the hub by now. */
		bool sawWithin(const char* what, int timeout_ms = 400)
		{
			pump([&] {
				fake.drain();
				return fake.received.find(what) != std::string::npos;
			}, timeout_ms);

			fake.drain();
			return fake.received.find(what) != std::string::npos;
		}
};

/*
 * We are never a source for ourselves, and never somebody to browse.
 *
 * Our own client ID reaches the queue by the ordinary routes - a search reply, and
 * the author of a bulletin post - so what is refused is the request itself.
 */
EXO_TEST(hubsession_our_own_share_is_not_asked_for,
{
	ManagedHub hub;
	if (!hub.fake.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(hub.ready);

	const QuickDC::User* me = hub.ui->getLocalUser();
	EXO_ASSERT_NOT_NULL(me);

	const char* mine = reinterpret_cast<const char*>(me->getClientID());
	EXO_ASSERT_NOT_NULL(mine);
	EXO_ASSERT(*mine);

	EXO_ASSERT(hub.ui->isLocalUser(mine));
	EXO_ASSERT(!hub.ui->isLocalUser("AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIIIJJK"));

	QuickDC::QueueManager* queue = core->queue;
	EXO_ASSERT_NOT_NULL(queue);

	const char* url = hub.ui->getUrl();
	EXO_ASSERT_NOT_NULL(url);

	/* Ourselves: nothing is queued, so nothing is ever connected to. */
	EXO_ASSERT(queue->requestFilelist(url, mine) == 0);
	EXO_ASSERT(queue->getFilelistRequest(url, mine) == 0);

	/* And a stranger on the same hub, so the refusal is about who and not about
	   the hub or the queue. */
	QuickDC::FilelistRequest* asked =
		queue->requestFilelist(url, "AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIIIJJK");
	EXO_ASSERT_NOT_NULL(asked);
	return 1;
});

/*
 * A post is looked for on the hub whose board it is on, and on no other.
 *
 * The hash of a post is on one hub's board, where every subscriber can read it. Asking
 * a second hub about it tells that operator which boards this client reads elsewhere,
 * which is a disclosure nothing about the download needs - and it asks people who could
 * not answer in any case, since a post is offered by hash alone and only by the readers
 * of that board.
 *
 * Both halves are here because only the pair says anything: that a search does go out,
 * and that the same search confined somewhere else goes nowhere.
 */
EXO_TEST(hubsession_a_post_is_looked_for_on_its_own_hub_and_no_other,
{
	ManagedHub hub;
	if (!hub.fake.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(hub.ready);

	QuickDC::QueueManager* queue = core->queue;
	EXO_ASSERT_NOT_NULL(queue);

	const char* post = "AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A";

	QuickDC::QueueFile* file = queue->add(scratch("post.txt").c_str(), post, 72);
	EXO_ASSERT_NOT_NULL(file);

	/* The canonical form the hub reports for itself, which is what is matched. */
	const char* here = hub.ui->getUrl();
	EXO_ASSERT_NOT_NULL(here);

	file->searchOnlyOn(here);

	if (!queue->requestSourceSearch(post))
	{
		queue->remove(file);
		EXO_FAIL("a post confined to a hub this client is on was not looked for");
	}

	if (!hub.sawWithin(post))
	{
		queue->remove(file);
		EXO_FAIL("a post confined to this hub was not looked for on it");
	}

	/* And confined to a hub this client is not on, it goes nowhere at all. */
	file->searchOnlyOn("adc://192.0.2.9:411");

	hub.fake.drain();
	hub.fake.received.clear();

	QuickDC::QueueManager::SourceSearchResult why =
		QuickDC::QueueManager::SourceSearchSent;

	const bool sent = queue->requestSourceSearch(post, &why);

	pump([] { return false; }, 200);
	hub.fake.drain();

	const bool leaked = hub.fake.received.find(post) != std::string::npos;

	queue->remove(file);

	/*
	 * Refused for want of a hub to ask, rather than sent to whoever was listening - and
	 * refused with its own answer rather than with NotSent, which means no hub at all is
	 * logged in and is the one answer that stops the rotation's pass. One file confined
	 * to a hub the user has left must not hold up the source search for every other
	 * download in the queue.
	 */
	EXO_ASSERT(!sent);
	EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchHubAbsent);

	if (leaked) EXO_FAIL("a post was looked for on a hub that is not its board's");

	return 1;
});

/*
 * And a file somebody asked about out of turn is not asked about again by the rotation.
 *
 * The two paths bound themselves separately, which is the point: the rotation sends one
 * search per interval for a queue nobody is watching, and the burst is what makes an
 * urgent ask urgent. But spending the burst does not leave the file needing to be asked
 * about again, and at start-up the rotation's interval has always elapsed - so the first
 * file of every session was searched for twice within the same second.
 */
EXO_TEST(hubsession_the_rotation_does_not_repeat_a_search_just_asked_for,
{
	ManagedHub hub;
	if (!hub.fake.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(hub.ready);

	QuickDC::QueueManager* queue = core->queue;
	EXO_ASSERT_NOT_NULL(queue);

	const char* post = "AAYFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A";

	QuickDC::QueueFile* file = queue->add(scratch("again.txt").c_str(), post, 72);
	EXO_ASSERT_NOT_NULL(file);

	file->searchOnlyOn(hub.ui->getUrl());

	if (!queue->requestSourceSearch(post) || !hub.sawWithin(post))
	{
		queue->remove(file);
		EXO_FAIL("the search that is not to be repeated did not go out");
	}

	const size_t after_the_ask = hub.fake.received.size();

	/*
	 * The rotation's turn, driven through the same call the queue's own tick makes
	 * rather than by reaching inside it. Its interval has elapsed - the queue has been
	 * up for the whole run - so the only thing that can hold it back is the file
	 * knowing it has just been searched for.
	 */
	queue->process();

	pump([] { return false; }, 200);
	hub.fake.drain();

	const bool repeated = hub.fake.received.size() != after_the_ask;

	queue->remove(file);

	if (repeated) EXO_FAIL("the rotation repeated a search that had just gone out");

	return 1;
});

/*
 * A file confined to a hub that is not here does not stop the rotation asking about the
 * others.
 *
 * Two answers that used to be one. "No hub is logged in" is a reason to stop the pass,
 * because nothing could go anywhere; "this file's own hub is not here" is a reason to
 * move on to the next file. Confining a file makes the second answer possible, and given
 * as the first it lets one post from a hub the user has left hold up the source search for
 * every other download in the queue - on every tick, since the pass returns without
 * resetting its interval.
 */
EXO_TEST(hubsession_a_file_whose_hub_is_gone_does_not_stop_the_others_being_searched_for,
{
	ManagedHub hub;
	if (!hub.fake.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(hub.ready);

	QuickDC::QueueManager* queue = core->queue;
	EXO_ASSERT_NOT_NULL(queue);

	/* One confined to a hub this client is not on, and one ordinary download. */
	const char* stranded = "AAZFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A";
	const char* ordinary = "AA2FLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A";

	QuickDC::QueueFile* lost = queue->add(scratch("lost.txt").c_str(), stranded, 72);
	QuickDC::QueueFile* other = queue->add(scratch("other.txt").c_str(), ordinary, 72);
	EXO_ASSERT_NOT_NULL(lost);
	EXO_ASSERT_NOT_NULL(other);

	lost->searchOnlyOn("adc://192.0.2.9:411");

	hub.fake.drain();
	hub.fake.received.clear();

	/*
	 * The rotation, taken from every starting point in turn: which file it lands on
	 * first is whatever the rotation happens to hold, and the stranded one must not stop
	 * the pass from whichever position it is reached.
	 */
	bool asked = false;

	for (int turn = 0; turn < 4 && !asked; turn++)
	{
		queue->process();
		asked = hub.sawWithin(ordinary, 200);
	}

	queue->remove(lost);
	queue->remove(other);

	if (!asked)
		EXO_FAIL("a file confined to an absent hub stopped the others being searched for");

	return 1;
});

/* ------------------------------------------------------------------------- */
/* What the session writes into the client's transcript of the hub           */
/* ------------------------------------------------------------------------- */

/*
 * The transcript is written where the chat is dispatched, so that it is kept
 * whether or not a front end is listening - a client with no window open on a
 * conversation is still in it. These drive real INF and MSG commands through a
 * real session and then read the file back.
 */
EXO_TEST(hubsession_public_chat_is_written_to_the_transcript,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(core->chat != 0);

	s.say("BINF BBBB IDBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB NIalice CT4");
	s.say("BMSG BBBB hello\\severyone");

	/* The canonical form, which is what a transcript is filed under. */
	const std::string hubUrl = s.ui->getUrl() ? s.ui->getUrl() : "";

	const std::vector<QuickDC::ChatArchive::Line> back =
		core->chat->tail(hubUrl, "", 10);

	EXO_ASSERT(back.size() == 1);
	EXO_ASSERT(back[0].nick == "alice");
	EXO_ASSERT(back[0].text == "hello everyone");

	/* Filed by the client id, which is what outlasts this session. */
	EXO_ASSERT(back[0].source == "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");

	/* An operator when they said it, and so an operator in the record. */
	EXO_ASSERT(back[0].op);
	EXO_ASSERT(!back[0].privateMessage);

	core->chat->forget(hubUrl, "");
	return 1;
});

/*
 * A private message is filed against whoever is not us, in both directions -
 * ours and theirs belong to the one conversation.
 */
EXO_TEST(hubsession_a_private_conversation_is_one_transcript_both_ways,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(core->chat != 0);

	s.say("BINF BBBB IDBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB NIalice");

	/* Theirs to us, and the echo of ours back to them. */
	s.say("EMSG BBBB AAAA hi\\sthere PMBBBB");
	s.say("EMSG AAAA BBBB hello\\sback PMBBBB");

	const std::string hubUrl = s.ui->getUrl() ? s.ui->getUrl() : "";
	const std::string with = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";

	const std::vector<QuickDC::ChatArchive::Line> back =
		core->chat->tail(hubUrl, with, 10);

	EXO_ASSERT(back.size() == 2);
	EXO_ASSERT(back[0].text == "hi there");
	EXO_ASSERT(back[1].text == "hello back");
	EXO_ASSERT(back[0].privateMessage);
	EXO_ASSERT(back[1].privateMessage);

	/* And none of it landed in the hub's own transcript. */
	EXO_ASSERT(core->chat->tail(hubUrl, "", 10).empty());

	core->chat->forget(hubUrl, with);
	return 1;
});

/*
 * TS says when a message was written. A hub replaying its history sends the
 * time each line was originally said, so a replay has to be written down under
 * those times rather than all at once at login - that is what lets it be lined
 * up against what this client already recorded.
 */
EXO_TEST(hubsession_a_hubs_timestamp_is_what_gets_recorded,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(core->chat != 0);

	s.say("BINF BBBB IDBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB NIalice");

	/* Two lines the hub says were written last year, as a replay would. */
	s.say("BMSG BBBB earlier TS1700000000");
	s.say("BMSG BBBB later TS1700000060");

	const std::string hubUrl = s.ui->getUrl() ? s.ui->getUrl() : "";

	const std::vector<QuickDC::ChatArchive::Line> back =
		core->chat->tail(hubUrl, "", 10);

	EXO_ASSERT(back.size() == 2);
	EXO_ASSERT(back[0].text == "earlier");
	EXO_ASSERT(back[0].time == 1700000000);
	EXO_ASSERT(back[1].text == "later");
	EXO_ASSERT(back[1].time == 1700000060);

	core->chat->forget(hubUrl, "");
	return 1;
});

/*
 * And a hub that sends none leaves the moment it arrived, so that every line
 * carries a stamp that can be compared with every other.
 */
EXO_TEST(hubsession_a_message_without_a_timestamp_is_stamped_on_arrival,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(core->chat != 0);

	const uint64_t before = (uint64_t) time(0);

	s.say("BINF BBBB IDBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB NIalice");
	s.say("BMSG BBBB no\\stimestamp\\shere");

	const uint64_t after = (uint64_t) time(0);

	const std::string hubUrl = s.ui->getUrl() ? s.ui->getUrl() : "";

	const std::vector<QuickDC::ChatArchive::Line> back =
		core->chat->tail(hubUrl, "", 10);

	EXO_ASSERT(back.size() == 1);
	EXO_ASSERT(back[0].text == "no timestamp here");

	/* Now, to the second, rather than zero or 1970. */
	EXO_ASSERT(back[0].time >= before);
	EXO_ASSERT(back[0].time <= after);

	core->chat->forget(hubUrl, "");
	return 1;
});

/* A TS that is not a number is no timestamp at all, and must not read as 1970. */
EXO_TEST(hubsession_an_unusable_timestamp_falls_back_to_arrival,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(core->chat != 0);

	const uint64_t before = (uint64_t) time(0);

	s.say("BINF BBBB IDBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB NIalice");
	s.say("BMSG BBBB rubbish\\sstamp TSnonsense");
	s.say("BMSG BBBB empty\\sstamp TS");

	const std::vector<QuickDC::ChatArchive::Line> back =
		core->chat->tail(s.ui->getUrl() ? s.ui->getUrl() : "", "", 10);

	EXO_ASSERT(back.size() == 2);

	for (size_t n = 0; n < back.size(); n++)
		EXO_ASSERT(back[n].time >= before);

	core->chat->forget(s.ui->getUrl() ? s.ui->getUrl() : "", "");
	return 1;
});

/*
 * The whole point of the timestamps: a hub that replays its history on login
 * hands back lines this client already wrote down. They must not be recorded
 * twice, and they must not be reported to the interface twice either - the
 * conversation on screen is filled from the same transcript as it opens.
 */
EXO_TEST(hubsession_a_replayed_line_is_neither_recorded_nor_reported_twice,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(core->chat != 0);

	const std::string hubUrl = s.ui->getUrl() ? s.ui->getUrl() : "";
	core->chat->forget(hubUrl, "");

	s.say("BINF BBBB IDBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB NIalice");

	/* What this client hears the first time round. */
	s.say("BMSG BBBB first TS1700000000");
	s.say("BMSG BBBB second TS1700000060");

	EXO_ASSERT(s.listener.chat.size() == 2);
	EXO_ASSERT(core->chat->tail(hubUrl, "", 100).size() == 2);

	const size_t reported = s.listener.chat.size();

	/*
	 * And what the hub replays on the next login: the two it already gave, and
	 * one said while this client was away.
	 */
	s.say("BMSG BBBB first TS1700000000");
	s.say("BMSG BBBB second TS1700000060");
	s.say("BMSG BBBB third TS1700000120");

	/* Only the one nobody had. */
	EXO_ASSERT(s.listener.chat.size() == reported + 1);
	EXO_ASSERT(s.listener.chat.back() == "third");

	const std::vector<QuickDC::ChatArchive::Line> back =
		core->chat->tail(hubUrl, "", 100);

	EXO_ASSERT(back.size() == 3);
	EXO_ASSERT(back[0].text == "first");
	EXO_ASSERT(back[1].text == "second");
	EXO_ASSERT(back[2].text == "third");

	core->chat->forget(hubUrl, "");
	return 1;
});

/*
 * Saying the same thing twice is not a replay. Without a hub timestamp each
 * line is stamped as it arrives, so the second is newer than the first and is
 * never compared with it.
 */
EXO_TEST(hubsession_saying_the_same_thing_twice_is_kept_twice,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	EXO_ASSERT(core->chat != 0);

	const std::string hubUrl = s.ui->getUrl() ? s.ui->getUrl() : "";
	core->chat->forget(hubUrl, "");

	s.say("BINF BBBB IDBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB NIalice");

	s.say("BMSG BBBB hello");
	s.say("BMSG BBBB hello");

	/*
	 * Both shown. They may share a stamp, having arrived in the same second,
	 * and that is exactly the case the record cannot tell from a replay - so
	 * the transcript is allowed to hold one or two of them, but the person
	 * watching must see both.
	 */
	EXO_ASSERT(s.listener.chat.size() >= 1);
	EXO_ASSERT(s.listener.chat.back() == "hello");

	core->chat->forget(hubUrl, "");
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Staying connected, and coming back                                        */
/* ------------------------------------------------------------------------- */

/*
 * Whether a keepalive is among what arrived.
 *
 * An empty ADC command is a line with nothing on it, so it is a newline either
 * at the very start of the stream or straight after the one that ended the
 * command before it. Looked for rather than assumed to be the only thing there:
 * the client sends its own info from time to time, and a case that asserted
 * nothing else had been said would fail whenever one of those landed in the
 * same second.
 */
static bool sent_a_keepalive(const std::string& stream)
{
	if (stream.empty()) return false;
	if (stream[0] == '\n') return true;

	return stream.find("\n\n") != std::string::npos;
}

/*
 * A connection nothing has crossed for a while is one a connection tracking
 * firewall quietly forgets, and the next thing sent over it is answered with a
 * reset rather than delivered. So a newline goes over an idle connection - ADC
 * reserves the empty command for exactly this - and finding the connection gone
 * costs a newline rather than something somebody typed.
 *
 * The interval is [Network] Keep alive interval, which is what lets this run in
 * a second rather than in the two minutes the default asks for.
 */
EXO_TEST(hubsession_an_idle_connection_is_kept_alive_with_a_newline,
{
	core->config->setGroup("Network");
	core->config->setNumber("Keep alive interval", 1);

	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	/*
	 * Nothing is said by either side from here. What arrives can only be the
	 * keepalive, and an empty ADC command is a line with nothing on it.
	 */
	const bool got = pump([&] {
		s.hub.drain();
		return sent_a_keepalive(s.hub.received);
	}, 4000);

	core->config->setGroup("Network");
	core->config->setNumber("Keep alive interval", 120);

	if (!got) EXO_FAIL("no keepalive arrived within four seconds");
	return 1;
});

/* Turned off by asking for none, for anyone who would rather find out the hard way. */
EXO_TEST(hubsession_a_zero_interval_sends_no_keepalive,
{
	core->config->setGroup("Network");
	core->config->setNumber("Keep alive interval", 0);

	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	pump([] { return false; }, 1500);
	s.hub.drain();

	core->config->setGroup("Network");
	core->config->setNumber("Keep alive interval", 120);

	/*
	 * No keepalive, rather than nothing at all: the client is entitled to send
	 * its own info in this window and that is not what this is about.
	 */
	EXO_ASSERT(!sent_a_keepalive(s.hub.received));
	return 1;
});

/*
 * The backoff has to start again after a connection that worked, or a client
 * idle on a hub for hours and then cut off waits out the ceiling to come back.
 *
 * The session is what remembers, because by the time the disconnection is
 * reported isLoggedIn() says no: every path that ends a session clears the
 * state first, which is why asking that question never reset anything.
 */
EXO_TEST(hubsession_a_session_remembers_that_it_logged_in,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	/* Connected, but not yet through the handshake. */
	EXO_ASSERT(!s.session->wasEstablished());

	EXO_ASSERT(s.login());

	EXO_ASSERT(s.session->wasEstablished());

	/*
	 * And it still says so once the state has been cleared, which is the whole
	 * point: this is read after the session has been torn down.
	 */
	s.session->disconnect();
	EXO_ASSERT(!s.session->isLoggedIn());
	EXO_ASSERT(s.session->wasEstablished());
	return 1;
});

/* How long is long enough to count as having worked. */
EXO_TEST(hubsession_only_a_connection_that_lasted_resets_the_backoff,
{
	EXO_ASSERT(!QuickDC::Hub::connectionHeld(0));
	EXO_ASSERT(!QuickDC::Hub::connectionHeld(1));
	EXO_ASSERT(!QuickDC::Hub::connectionHeld(QUICKDC_HUB_STABLE - 1));

	EXO_ASSERT(QuickDC::Hub::connectionHeld(QUICKDC_HUB_STABLE));
	EXO_ASSERT(QuickDC::Hub::connectionHeld(QUICKDC_HUB_STABLE + 1));
	EXO_ASSERT(QuickDC::Hub::connectionHeld(100000));
	return 1;
});

/* ------------------------------------------------------------------------- */

/*
 * Each family keeps its own evidence, because each is its own endpoint.
 *
 * A client can be reached natively over IPv6 while every IPv4 attempt dies in a
 * carrier-grade NAT, so one record standing in for the other would report one
 * family's luck as the other's - and the claim a hub is given is per family.
 */
EXO_TEST(hubsession_the_two_families_keep_their_own_evidence,
{
	EXO_ASSERT(core != 0);

	EXO_ASSERT(core->reachability != 0);
	EXO_ASSERT(core->reachability6 != 0);
	EXO_ASSERT(core->reachability != core->reachability6);

	/* And the one a caller holding an address asks for is the one it means. */
	EXO_ASSERT(core->reachabilityFor(false) == core->reachability);
	EXO_ASSERT(core->reachabilityFor(true) == core->reachability6);

	core->reachability->forget();
	core->reachability6->forget();

	core->reachability6->advertising("2001:db8::5", 1411);
	core->reachability6->inboundConnection();

	EXO_ASSERT(core->reachability6->tcp() == QuickDC::Reachability::State::Reachable);
	EXO_ASSERT(core->reachability->tcp() == QuickDC::Reachability::State::Unknown);

	/* And the same the other way, so neither is merely the one that was written. */
	core->reachability6->forget();
	core->reachability->advertising("198.51.100.9", 1411);
	core->reachability->inboundDatagram();

	EXO_ASSERT(core->reachability->udp() == QuickDC::Reachability::State::Reachable);
	EXO_ASSERT(core->reachability6->udp() == QuickDC::Reachability::State::Unknown);

	core->reachability->forget();
	return 1;
});

/*
 * A client that does not know its own IPv6 address claims no IPv6 endpoint.
 *
 * There is no wildcard for I6 the way "0.0.0.0" asks a hub to fill in I4, so the
 * only honest INF is one that says nothing: naming an address no peer can reach
 * has every one of them try it and fail, where saying nothing has them use the
 * family that works. And TCP6 is a claim peers act on, so it must not be made
 * merely because the client supports the field.
 */
EXO_TEST(hubsession_no_ipv6_endpoint_is_claimed_before_one_is_known,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	ActiveCore active;
	EXO_ASSERT(s.login());

	std::string inf;
	EXO_ASSERT(announcedSupport(s, inf));

	EXO_ASSERT(!claims(inf, "TCP6"));
	EXO_ASSERT(!claims(inf, "UDP6"));

	/*
	 * And the field itself carries nothing. Sent empty rather than left out, which
	 * is what withdraws one announced while we did know - so what is checked is
	 * that no address went out with it, not that the field is absent.
	 */
	const size_t at = inf.find(" I6");
	if (at != std::string::npos)
	{
		const size_t after = at + 3;
		EXO_ASSERT(after >= inf.size() || inf[after] == ' ');
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Following the identity in the settings                                    */
/* ------------------------------------------------------------------------- */

namespace {

/* Puts a [User] setting back as it was, since every case here shares the core. */
class UserSetting {
	public:
		explicit UserSetting(const char* key)
			: key(key)
			, had(core->config->haveSetting("User", key))
			, was(core->config->peekString("User", key, ""))
		{
		}

		~UserSetting()
		{
			if (had) core->config->setString("User", key, was);
			else core->config->remove("User", key);
		}

	private:
		const char* key;
		bool had;
		std::string was;
};

/* Answer a client's HSUP and INF on 'fake' until it is logged in; its INF is returned. */
std::string loginOn(FakeHub& fake, const char* sid = "AAAA")
{
	std::string line;
	if (!fake.nextLine(line) || line.compare(0, 4, "HSUP") != 0) return std::string();
	fake.send("ISUP ADBASE ADTIGR");
	pump([] { return false; }, 60);
	fake.send(std::string("ISID ") + sid);
	pump([] { return false; }, 60);
	if (!fake.nextLine(line) || line.compare(0, 4, "BINF") != 0) return std::string();
	fake.send(line);
	pump([] { return false; }, 60);
	return line;
}

}

/* A description changed while logged in goes out with the next INF, and nothing reconnects. */
EXO_TEST(hubsession_a_changed_description_goes_out_without_reconnecting,
{
	UserSetting keep("Description");
	core->config->setString("User", "Description", "before");

	FakeHub fake;
	Listener listener;
	if (!fake.listen()) EXO_SKIP("could not listen on a loopback port");

	Samurai::IO::Net::URL url("adc://127.0.0.1:" + std::to_string(fake.port()));
	QuickDC::Hub ui(*core, &listener, &url);
	ui.connect();
	EXO_ASSERT(fake.accepted());
	EXO_ASSERT(infField(loginOn(fake), "DE") == "before");

	const auto first = fake.peer;
	fake.drain();
	fake.received.clear();

	core->config->setString("User", "Description", "after");
	ui.refreshIdentity();
	core->hubs->sendClientInfoToAllHubs();

	std::string line;
	EXO_ASSERT(fake.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "BINF") == 0);
	EXO_ASSERT(infField(line, "DE") == "after");
	EXO_ASSERT(fake.peer == first);
	EXO_ASSERT(!fake.disconnected);
	return 1;
});

/*
 * A nickname changed while logged in reconnects under the new one: a hub knows a
 * logged-in user by it, and many refuse to see it change.
 */
EXO_TEST(hubsession_a_changed_nickname_reconnects_under_it,
{
	UserSetting keep("Nickname");
	core->config->setString("User", "Nickname", "first-name");

	FakeHub fake;
	Listener listener;
	if (!fake.listen()) EXO_SKIP("could not listen on a loopback port");

	Samurai::IO::Net::URL url("adc://127.0.0.1:" + std::to_string(fake.port()));
	QuickDC::Hub ui(*core, &listener, &url);
	ui.connect();
	EXO_ASSERT(fake.accepted());
	EXO_ASSERT(infField(loginOn(fake), "NI") == "first-name");

	const auto first = fake.peer;
	core->config->setString("User", "Nickname", "second-name");
	ui.refreshIdentity();

	EXO_ASSERT(pump([&] { return fake.peer && fake.peer != first; }));
	fake.received.clear();
	EXO_ASSERT(infField(loginOn(fake, "BBBB"), "NI") == "second-name");
	return 1;
});

/* The name a session used is not left behind as this hub's own, outranking the setting. */
EXO_TEST(hubsession_a_closed_session_leaves_no_nickname_behind,
{
	UserSetting keep("Nickname");
	core->config->setString("User", "Nickname", "global-name");

	std::string group;
	{
		Session s;
		if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
		EXO_ASSERT(s.ready);
		EXO_ASSERT(s.login());
		group = "adc://127.0.0.1:" + std::to_string(s.hub.port());
	}

	EXO_ASSERT(!core->session->haveSetting(group, "Nickname"));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Asking a queued source to connect                                         */
/* ------------------------------------------------------------------------- */

namespace {

const char SOURCE_CID[] = "SOURCEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const char SOURCE_TTH[] = "SOURCETTHAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

/* Whether 'fake' was sent a connection request - either way round - since 'before'. */
bool askedToConnect(FakeHub& fake)
{
	fake.drain();
	return fake.received.find("DCTM") != std::string::npos
	    || fake.received.find("DRCM") != std::string::npos;
}

/* A file in the queue with one source, recorded against 'hubUrl'; taken out again on scope exit. */
class QueuedFromSource {
	public:
		explicit QueuedFromSource(const std::string& hubUrl)
		{
			file = core->queue->add(scratch("queued.bin").c_str(), SOURCE_TTH, 64 * 1024);
			if (file) file->addSource(new QuickDC::FileSource(hubUrl.c_str(), SOURCE_CID));
		}

		~QueuedFromSource()
		{
			if (file) core->queue->remove(file);
		}

		QuickDC::FileSource* source() { return file ? file->getSource(0) : nullptr; }

		QuickDC::QueueFile* file = nullptr;
};

}

/*
 * A source whose user is on no hub we are on is not asked for, and that is not
 * counted against it - it used to be a request made and dropped several times a
 * second, for ever.
 */
EXO_TEST(hubsession_a_source_on_no_hub_is_left_alone_without_counting_against_it,
{
	FakeHub fake;
	Listener listener;
	if (!fake.listen()) EXO_SKIP("could not listen on a loopback port");

	const std::string address = "adc://127.0.0.1:" + std::to_string(fake.port());
	Samurai::IO::Net::URL url(address);
	QuickDC::Hub ui(*core, &listener, &url);
	ui.connect();
	EXO_ASSERT(fake.accepted());
	EXO_ASSERT(!loginOn(fake).empty());
	fake.drain();
	fake.received.clear();

	QueuedFromSource queued(address);
	EXO_ASSERT(queued.source() != nullptr);

	for (int n = 0; n < 5; n++) core->queue->process();
	pump([] { return false; }, 100);

	EXO_ASSERT(!askedToConnect(fake));
	EXO_ASSERT(!queued.source()->isReadyToRetry());
	EXO_ASSERT_EQ_UINT(queued.source()->getRetryCount(), 0u);
	return 1;
});

/*
 * A client ID is the same on every hub, so a source whose user has left the hub it
 * was found on, and is on another we are on, is asked there.
 */
EXO_TEST(hubsession_a_source_that_moved_hubs_is_asked_where_it_is,
{
	FakeHub first;
	FakeHub second;
	Listener listener;
	if (!first.listen() || !second.listen()) EXO_SKIP("could not listen on a loopback port");

	const std::string firstUrl = "adc://127.0.0.1:" + std::to_string(first.port());
	const std::string secondUrl = "adc://127.0.0.1:" + std::to_string(second.port());
	Samurai::IO::Net::URL a(firstUrl);
	Samurai::IO::Net::URL b(secondUrl);
	QuickDC::Hub uiA(*core, &listener, &a);
	QuickDC::Hub uiB(*core, &listener, &b);

	uiA.connect();
	EXO_ASSERT(first.accepted());
	EXO_ASSERT(!loginOn(first, "AAAA").empty());

	uiB.connect();
	EXO_ASSERT(second.accepted());
	EXO_ASSERT(!loginOn(second, "AAAB").empty());

	second.send(std::string("BINF CCCC ID") + SOURCE_CID + " NIelsewhere SS1024 SL3 I4192.0.2.7 U41412");
	pump([] { return false; }, 100);
	EXO_ASSERT(uiB.hasSourceUser(SOURCE_CID));
	EXO_ASSERT(!uiA.hasSourceUser(SOURCE_CID));

	first.drain();
	first.received.clear();
	second.drain();
	second.received.clear();

	QueuedFromSource queued(firstUrl);
	EXO_ASSERT(queued.source() != nullptr);

	core->queue->process();
	pump([&] { return askedToConnect(second); }, 2000);

	EXO_ASSERT(askedToConnect(second));
	EXO_ASSERT(second.received.find("CCCC") != std::string::npos);
	EXO_ASSERT(!askedToConnect(first));
	return 1;
});

namespace {

bool anyHubKeepsPassword()
{
	for (const std::string& group : core->session->groupNames())
		if (group.find("://") != std::string::npos && core->session->haveSetting(group, "Password")) return true;
	return false;
}

}

/*
 * A password the hub refused is forgotten rather than offered again on the next
 * run, only to be refused the same way.
 */
EXO_TEST(hubsession_a_refused_password_is_forgotten,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	s.say("ISUP ADBASE");
	s.say("ISID AAAA");
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("IGPA ABCDEFGHIJKLMNOPQRSTUVWXYZ");
	EXO_ASSERT(s.hub.nextLine(line));

	s.session->sendPassword("wrong-adc");
	EXO_ASSERT(anyHubKeepsPassword());

	s.say("ISTA 223 Invalid\\spassword");
	EXO_ASSERT(pump([] { return !anyHubKeepsPassword(); }));
	return 1;
});

/* The password every hub is given, typed in again for one, is not kept as that hub's own. */
EXO_TEST(hubsession_the_shared_password_typed_in_is_not_pinned_to_the_hub,
{
	core->config->setString("User", "Password", "shared-adc");

	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	s.say("ISUP ADBASE");
	s.say("ISID AAAA");
	EXO_ASSERT(s.hub.nextLine(line));

	s.say("IGPA ABCDEFGHIJKLMNOPQRSTUVWXYZ");
	EXO_ASSERT(s.hub.nextLine(line));

	s.session->sendPassword("shared-adc");
	EXO_ASSERT(!anyHubKeepsPassword());

	core->config->remove("User", "Password");
	return 1;
});

/*
 * And the dial itself goes out from that port, which only works if the hub
 * connection was made shareable when it was opened: on Linux a second socket may
 * bind a port in use only where every socket on it allowed that. The hub
 * connection never did, so every traversal failed to bind before it dialled, and
 * the tests above - which stop at the answer - could not see it.
 */
EXO_TEST(hubsession_a_traversal_dials_from_the_hub_connections_own_port,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");

	FakeHub other;
	if (!other.listen()) EXO_SKIP("could not listen on a second loopback port");

	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	s.say("BINF BBBB NIpeer I4127.0.0.1 SS100 SL1 SUADC0,NATT");

	setRequireEncryption(false);
	s.say("DNAT BBBB AAAA ADC/1.0 " + std::to_string(other.port()) + " job7");

	EXO_ASSERT(other.accepted());

	EXO_ASSERT(s.hub.peer != 0 && other.peer != 0);
	EXO_ASSERT(other.peer->getPort() == s.hub.peer->getPort());
	return 1;
});
