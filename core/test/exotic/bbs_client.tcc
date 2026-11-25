/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * What a client says and hears about bulletin boards, over a real socket.
 *
 * The same arrangement as hubsession.tcc: a real ADC::HubSession, its own command
 * proxy and the real event loop, talking to a hub that is a listening socket on a
 * loopback port the kernel picks. Nothing here fakes the protocol layer, so what
 * is asserted is what would go over the wire - which is the whole point for an
 * extension whose commands nobody else in this tree sends.
 *
 * The core is the one thing that is not real. The full one creates ~/.quickdc,
 * reads the user's configuration and scans the shares; a session needs none of
 * that, so this uses the constructor that builds the four pieces it does need,
 * pointed at scratch files.
 */

#include "quickdc.h"
#include "api/bulletins.h"
#include "api/core.h"
#include "api/hub.h"
#include "network/hubmanager.h"
#include "network/adc/adcbbs.h"
#include "network/adc/adccommon.h"
#include "network/adc/client/adchubsession.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "config/preferences.h"

#include <samurai/io/dir.h>
#include <samurai/io/file.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/tlsfactory.h>
#include <samurai/io/net/url.h>
#include <samurai/timer.h>

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
	return "/tmp/quickdc_bbstest_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

static const int TIMEOUT_MS = 5000;

/* A hash of the right shape: thirty-nine characters of upper case base32. */
static const char* TTH_A = "KX3TQ7ZVN5PLQGKD3NDBK6ZTZG5PYQXSNMFYVJH";
static const char* TTH_B = "7ZQGKD3NDBK6ZTZG5PYQXSNMFYVJH4TXAVN6PLQ";
static const char* CID_A = "IPJJWEPPPLCA3PF2ZCRRYO4F2ZX2EV2JMW2KC3I";

/* ------------------------------------------------------------------------- */

/* Runs the event loop until the condition holds, or gives up. */
template<typename Fn>
static bool pump(Fn done, int timeout_ms = TIMEOUT_MS)
{
	Samurai::IO::Net::SocketMonitor* monitor = &core->loop().monitor();
	Samurai::TimerManager* timers = &core->loop().timers();

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);

	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		monitor->wait(10);
		timers->process();
	}
	return true;
}

/* Records what the session told the interface about boards. */
class Listener : public QuickDC::HubListener
{
	public:
		/** One entry per refusal, in the order they were reported. */
		struct Refusal
		{
			int code = 0;
			std::string message;
			std::string board;
			std::string tth;
		};

		/*
		 * Every statement of availability, and not merely the last one: the
		 * notification says what is true now rather than what changed, so a case
		 * about a feature being withdrawn is about the sequence.
		 */
		std::vector<bool> availability;

		std::vector<QuickDC::Board> boards;
		std::vector<QuickDC::BoardEntry> entries;
		std::vector<Refusal> refusals;
		std::vector<std::string> errors;

		void EventNetStatus(enum StatusNetwork) override { }
		void EventHubStatus(enum StatusHub) override { }

		void EventSystemError(const char* msg) override
		{ errors.push_back(msg ? msg : ""); }

		void EventChat(const QuickDC::User*, const char*, bool, bool, uint64_t) override { }
		void EventPrivateChat(const QuickDC::User*, const QuickDC::User*, const char*,
			const char*, bool, bool, uint64_t) override { }
		void EventHubMessage(const char*, const char*, bool, bool, uint64_t) override { }
		void EventUserJoin(const char*) override { }
		void EventUserLeave(const QuickDC::User*, const char*, bool) override { }
		void EventUsersCleanup() override { }
		void EventUserUpdate(const QuickDC::User*) override { }
		void EventSearchResult(void*) override { }
		void EventHubName(const char*) override { }

		void EventBoardsAvailable(bool available) override
		{ availability.push_back(available); }

		void EventBoardDescribed(const QuickDC::Board& board) override
		{ boards.push_back(board); }

		void EventBoardEntry(const QuickDC::BoardEntry& entry) override
		{ entries.push_back(entry); }

		void EventBoardRefused(int code, const char* message, const char* board,
			const char* tth) override
		{
			Refusal refusal;
			refusal.code = code;
			refusal.message = message ? message : "";
			refusal.board = board ? board : "";
			refusal.tth = tth ? tth : "";
			refusals.push_back(refusal);
		}

		void EventHubRedirect(const char*, uint16_t, const char*, QuickDC::RedirectRefusal) override { }
		void EventHubAutenticate() override { }

		bool EventClientConnect(const char*, uint16_t, const QuickDC::User*) override
		{ return false; }
		bool EventClientConnect(const QuickDC::User*) override { return false; }
};

/*
 * The other end of the connection: a listening socket, and whatever the client
 * sent, kept as a stream so it can be read a line at a time.
 *
 * Optionally the encrypted end of one. A client that believes it is on an ADCS hub
 * opens with a handshake rather than with HSUP, so the one case about what such a
 * client offers cannot be answered by a plain socket at all - it would be
 * asserting something about the bytes of a ClientHello.
 */
class FakeHub :
	public Samurai::IO::Net::ServerSocketEventHandler,
	public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> peer;
		std::string received;

		/* Whether the port could be bound at all, which is what tells a machine
		   that cannot apart from a client that will not connect. */
		bool listening = false;

		/** Whether this end answers a TLS handshake, and whether one finished. */
		bool secure = false;
		bool established = false;

		explicit FakeHub(bool tls = false) : secure(tls) { }

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(core->loop(), this, any);
			listening = server && server->getFD() != -1 && server->listen();
			return listening;
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }

		/* Waits for the client's connection to be accepted, and for the handshake
		   where there is one to answer. */
		bool accepted()
		{
			if (!pump([&] { return peer != 0; })) return false;
			if (!secure) return true;
			return pump([&] { return established; });
		}

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

			if (!secure) return;

			/*
			 * One pass rarely completes a handshake; the socket resumes it as the
			 * connection becomes readable or writable again and reports the result
			 * through EventTLSConnected(), which is what accepted() waits for.
			 */
			if (peer->TLSInitialize(true)) peer->TLSsendHandshake();
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

		void EventTLSConnected(const Samurai::IO::Net::Socket*) override
		{ established = true; }

		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override { }
};

/*
 * A hub, and a session connected to it. Every case starts here and drives the
 * login exchange itself, because what the hub says in its ISUP is what most of
 * them are about.
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
		 * @param secure whether both ends speak TLS, which for the client means it
		 *        opens with a handshake instead of with HSUP.
		 */
		explicit Session(bool secure = false) : hub(secure)
		{
			if (!hub.listen()) return;

			const std::string address = std::string(secure ? "adcs" : "adc")
				+ "://127.0.0.1:" + std::to_string(hub.port());
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

		/**
		 * The support exchange, a SID and the INF echoed back, which is what
		 * reaching the NORMAL state means.
		 *
		 * @param boards whether the hub announces BBS0 in its ISUP. A hub that
		 *        declines a bulletin board does so by saying nothing, so this is
		 *        the whole difference between a hub with boards and one without.
		 */
		bool login(bool boards = true)
		{
			std::string line;
			if (!hub.nextLine(line)) return false;
			if (line.compare(0, 4, "HSUP") != 0) return false;

			say(std::string("ISUP ADBASE ADTIGR")
				+ (boards ? " AD" ADC_EXT_BULLETIN : ""));
			say("ISID AAAA");
			say("IINF NITestHub");

			if (!hub.nextLine(line) || line.compare(0, 4, "BINF") != 0) return false;
			say(line);
			return true;
		}

		/* What the client sent, with the login exchange thrown away. */
		void forgetTraffic()
		{
			hub.drain();
			hub.received.clear();
		}
};

/*
 * A certificate for the two ends of the TLS case to present.
 *
 * The fixture core makes none - it is the constructor meant for a test, and it
 * stays out of the user's own ssl directory - so a handshake has nothing to offer
 * without this. Pointed away again afterwards, so that no other case is affected
 * by there being one.
 */
class Certificate
{
	public:
		Certificate()
		{
			key = scratch("hub.key");
			crt = scratch("hub.crt");

			Samurai::IO::Net::TlsFactory::generateSelfSignedCertificate(
				key.c_str(), crt.c_str(), QUICKDC_CERTIFICATE_NAME);

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

			Samurai::IO::File(key).remove();
			Samurai::IO::File(crt).remove();
		}

		Certificate(const Certificate&) = delete;
		Certificate& operator=(const Certificate&) = delete;

		bool usable = false;

	private:
		std::string key;
		std::string crt;
};

/* ------------------------------------------------------------------------- */

namespace {

/*
 * The fixture. One core for the whole process, because it is expensive to build
 * and has to be pointed at scratch files rather than the user's own
 * configuration.
 */
std::string config_file;
std::string session_file;
std::string chat_dir;

/*
 * The value of one named field of a command, or nothing where it is absent.
 *
 * Looked up token by token rather than by searching the line, because the values
 * here are base32 - so "TS", "BD" and "RM" all occur inside a hash from time to
 * time, and a search for a bare field name is a test that passes most days.
 */
std::string field(const std::string& line, const std::string& name)
{
	size_t at = 0;
	while (at < line.size())
	{
		size_t end = line.find(' ', at);
		if (end == std::string::npos) end = line.size();

		const std::string token = line.substr(at, end - at);
		if (token.size() >= name.size() && token.compare(0, name.size(), name) == 0)
			return token.substr(name.size());

		at = end + 1;
	}
	return std::string();
}

/** Whether a command carries a named field at all. */
bool hasField(const std::string& line, const std::string& name)
{
	size_t at = 0;
	while (at < line.size())
	{
		size_t end = line.find(' ', at);
		if (end == std::string::npos) end = line.size();

		const std::string token = line.substr(at, end - at);
		if (token.size() >= name.size() && token.compare(0, name.size(), name) == 0)
			return true;

		at = end + 1;
	}
	return false;
}

/** One index entry as a hub would send it, with everything a post has. */
std::string entryLine(const char* tth, const char* board, uint64_t timestamp)
{
	return std::string("IBBL TR") + tth + " SI412 BD" + board + " ID" + CID_A
		+ " NIjanvidar TH" + tth + " SJa\\ssubject TS" + std::to_string(timestamp);
}

/** Whether every sender refuses, which is what a hub without boards leaves. */
bool allSendersRefuse(Session& s)
{
	if (s.session->subscribeBoard("general", 0)) return false;
	if (s.session->unsubscribeBoard("general")) return false;
	if (s.session->requestEntry("general", TTH_A)) return false;
	if (s.session->submitPost(TTH_A, 412, "general", "a subject", 0)) return false;
	if (s.session->withdrawPost(TTH_A, "general")) return false;
	return true;
}

}

EXO_SETUP({
	config_file = scratch("config.conf");
	session_file = scratch("session.conf");
	chat_dir = scratch("chat");

	core = new QuickDC::Core(config_file, session_file, std::string(), chat_dir);
	return 1;
});

EXO_TEARDOWN({
	/* Preferences writes itself out when it is destroyed, so removing the files
	   has to wait until the core holding them is gone. */
	delete core;
	core = 0;

	Samurai::IO::File(config_file).remove();
	Samurai::IO::File(session_file).remove();

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
/* offering the feature                                                      */
/* ------------------------------------------------------------------------- */

/*
 * A client that implements this extension offers it in the HSUP it opens with.
 *
 * Worth a case of its own for the reason RTF0 is: a hub sends nothing about a
 * board to a client that did not ask, so a flag going missing here is not an
 * error anywhere - it is a client on which boards silently do not exist.
 */
EXO_TEST(bbs_client_the_hsup_offers_bulletin_boards,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HSUP") == 0);

	EXO_ASSERT(line.find("AD" ADC_EXT_BULLETIN) != std::string::npos);

	/* And beside the ones a board rests on: every identifier in the extension is
	   a tiger tree root, and a post body is fetched like any other file. */
	EXO_ASSERT(line.find("AD" ADC_HASH_TIGER) != std::string::npos);
	EXO_ASSERT(line.find("ADBASE") != std::string::npos);
	return 1;
});

/*
 * And on the encrypted path, which is a separate HSUP written at a separate place:
 * a client that believes it is on an ADCS hub sends nothing at all until the
 * handshake is done, so the flag has to be added twice and this is the half that
 * no plain socket can be asked about.
 */
EXO_TEST(bbs_client_the_hsup_offers_bulletin_boards_over_tls,
{
	Certificate certificate;
	if (!certificate.usable) EXO_SKIP("could not generate a certificate");

	Session s(true);
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	if (!s.ready) EXO_SKIP("the TLS handshake did not complete");

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HSUP") == 0);
	EXO_ASSERT(line.find("AD" ADC_EXT_BULLETIN) != std::string::npos);
	return 1;
});

/*
 * A hub that says nothing has no boards, and the client's own offer changes
 * nothing about that: the hub's answer is authoritative for the connection, so
 * every sender refuses and nothing goes out.
 */
EXO_TEST(bbs_client_a_quiet_hub_leaves_every_sender_refusing,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login(false));

	EXO_ASSERT(!s.session->supportsBulletins());

	s.forgetTraffic();
	EXO_ASSERT(allSendersRefuse(s));

	pump([] { return false; }, 120);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.empty());

	/* And the interface was told, rather than left to guess from an absence: what
	   is reported is the current answer after every SUP, which here is 'no'. */
	EXO_ASSERT(!s.listener.availability.empty());
	EXO_ASSERT(!s.listener.availability.back());
	return 1;
});

/*
 * A hub may announce the feature in a later SUP than its first, and may take it
 * back with RM. Both are ordinary - a hub that has lost whatever it needed to
 * serve a board has no other way of saying so - and both are reported, because
 * there is no shape in an announcement of what a hub supports that means
 * "withdrawn".
 */
EXO_TEST(bbs_client_a_later_isup_turns_the_senders_on_and_off_again,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login(false));

	EXO_ASSERT(!s.session->supportsBulletins());
	const size_t said = s.listener.availability.size();

	s.say("ISUP AD" ADC_EXT_BULLETIN);

	EXO_ASSERT(s.session->supportsBulletins());
	EXO_ASSERT(s.listener.availability.size() == said + 1);
	EXO_ASSERT(s.listener.availability.back());

	s.forgetTraffic();
	EXO_ASSERT(s.session->subscribeBoard("general", 0));

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HBBL") == 0);

	s.say("ISUP RM" ADC_EXT_BULLETIN);

	EXO_ASSERT(!s.session->supportsBulletins());
	EXO_ASSERT(s.listener.availability.size() == said + 2);
	EXO_ASSERT(!s.listener.availability.back());

	/* Every subscription went with the feature, so nothing may be said about a
	   board until the hub offers it again. */
	s.forgetTraffic();
	EXO_ASSERT(allSendersRefuse(s));

	pump([] { return false; }, 120);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.empty());
	return 1;
});

/*
 * Nothing is sent before the session reaches the NORMAL state either, however the
 * hub answered. A board is not a thing that exists during a login: the hub has no
 * user to attribute a post to yet, and this client has not been accepted.
 */
EXO_TEST(bbs_client_nothing_is_said_about_a_board_before_the_login_finishes,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	s.say("ISUP ADBASE ADTIGR AD" ADC_EXT_BULLETIN);

	/* The hub has offered boards, so the feature is there and only the state is
	   missing - which is what makes this about the state. */
	EXO_ASSERT(s.session->supportsBulletins());
	EXO_ASSERT(!s.session->isLoggedIn());

	s.forgetTraffic();
	EXO_ASSERT(allSendersRefuse(s));

	pump([] { return false; }, 120);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.empty());

	/*
	 * And nothing is heard either. A hub describes its boards on a session's entry
	 * to the NORMAL state, so a descriptor arriving before that is a hub putting a
	 * board in front of a user it has not accepted yet - at a point where nothing
	 * said back about that board could be answered.
	 */
	s.say("IBBD BDgeneral PE15 MS262144 TS1786439662 OT0 NP7");
	s.say(entryLine(TTH_A, "general", 1786436000));

	EXO_ASSERT(s.listener.boards.empty());
	EXO_ASSERT(s.listener.entries.empty());
	return 1;
});

/* ------------------------------------------------------------------------- */
/* what the hub says there is                                                */
/* ------------------------------------------------------------------------- */

EXO_TEST(bbs_client_a_descriptor_is_parsed_and_reported,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("IBBD BDgeneral NIGeneral\\sdiscussion DEAnything\\sabout\\sthis\\shub "
	      "PE15 MS262144 TS1786439662 OT1767225600 NP311");

	EXO_ASSERT(s.listener.boards.size() == 1);
	if (s.listener.boards.empty()) return 0;

	const QuickDC::Board& board = s.listener.boards.front();
	EXO_ASSERT(board.name == "general");
	EXO_ASSERT(board.title == "General discussion");
	EXO_ASSERT(board.description == "Anything about this hub");
	EXO_ASSERT(board.maxSize == 262144);
	EXO_ASSERT(board.newest == 1786439662);
	EXO_ASSERT(board.oldest == 1767225600);
	EXO_ASSERT(board.posts == 311);
	EXO_ASSERT(!board.removed);

	/* 15 is 1+2+4+8: subscribe, post, reply and withdraw one's own. */
	EXO_ASSERT(board.may(QuickDC::Subscribe));
	EXO_ASSERT(board.may(QuickDC::StartThread));
	EXO_ASSERT(board.may(QuickDC::Reply));
	EXO_ASSERT(board.may(QuickDC::WithdrawOwn));
	EXO_ASSERT(!board.may(QuickDC::WithdrawAny));
	return 1;
});

/* A board that has ceased to exist is a descriptor carrying RM1 and little else. */
EXO_TEST(bbs_client_a_removed_board_is_reported_as_removed,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("IBBD BDgeneral RM1");

	EXO_ASSERT(s.listener.boards.size() == 1);
	EXO_ASSERT(!s.listener.boards.empty() && s.listener.boards.front().removed);
	EXO_ASSERT(!s.listener.boards.empty() && s.listener.boards.front().name == "general");
	return 1;
});

/*
 * The backlog and the live feed are one stream, so a burst of entries is reported
 * one by one and in the order it arrived. Order is the hub's to state and this
 * side's to preserve: it is what a board is sorted by and what a subscription
 * resumes from.
 */
EXO_TEST(bbs_client_a_burst_of_entries_is_reported_in_order,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say(entryLine(TTH_A, "general", 1786436000));
	s.say(entryLine(TTH_B, "general", 1786438120));
	s.say(entryLine(TTH_A, "general", 1786438120));

	EXO_ASSERT(s.listener.entries.size() == 3);
	if (s.listener.entries.size() != 3) return 0;

	EXO_ASSERT(s.listener.entries[0].tth == TTH_A);
	EXO_ASSERT(s.listener.entries[0].timestamp == 1786436000);
	EXO_ASSERT(s.listener.entries[0].size == 412);
	EXO_ASSERT(s.listener.entries[0].board == "general");
	EXO_ASSERT(s.listener.entries[0].cid == CID_A);
	EXO_ASSERT(s.listener.entries[0].nick == "janvidar");
	EXO_ASSERT(s.listener.entries[0].subject == "a subject");

	EXO_ASSERT(s.listener.entries[1].tth == TTH_B);
	EXO_ASSERT(s.listener.entries[1].timestamp == 1786438120);

	/*
	 * A second entry for a hash already seen is passed on rather than swallowed
	 * here. Two of them are ordinary - the second of a resumed subscription, and a
	 * tombstone - and telling them apart is a question about what the receiver
	 * already holds, which is not this layer's to answer.
	 */
	EXO_ASSERT(s.listener.entries[2].tth == TTH_A);
	EXO_ASSERT(s.listener.entries[2].timestamp == 1786438120);
	return 1;
});

/* A tombstone carries the hash, the board, a new timestamp and the flag. */
EXO_TEST(bbs_client_a_tombstone_is_reported_as_a_withdrawal,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say(std::string("IBBL TR") + TTH_A + " BDgeneral TS1786443100 RM1");

	EXO_ASSERT(s.listener.entries.size() == 1);
	if (s.listener.entries.empty()) return 0;

	EXO_ASSERT(s.listener.entries[0].removed);
	EXO_ASSERT(s.listener.entries[0].tth == TTH_A);
	EXO_ASSERT(s.listener.entries[0].timestamp == 1786443100);

	/* And it is not refused for lacking what a post has: the subject and author of
	   a withdrawn post are usually the reason it was withdrawn. */
	EXO_ASSERT(s.listener.entries[0].subject.empty());
	EXO_ASSERT(s.listener.entries[0].cid.empty());
	return 1;
});

/*
 * A descriptor or an entry from a hub that never announced BBS0 is ignored, which
 * the extension requires. A hub declines a bulletin board by saying nothing, and
 * one that then sends a board anyway is announcing something to a client it never
 * agreed to speak this to - a descriptor is what puts a board in front of a user,
 * and an entry is a claim about a post nobody asked about.
 */
EXO_TEST(bbs_client_a_descriptor_from_a_hub_that_never_offered_boards_is_ignored,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login(false));

	s.say("IBBD BDgeneral PE15 MS262144 TS1786439662 OT0 NP7");
	s.say(entryLine(TTH_A, "general", 1786436000));

	EXO_ASSERT(s.listener.boards.empty());
	EXO_ASSERT(s.listener.entries.empty());

	/* And the same hub is believed the moment it does announce it, the answer
	   being what it currently claims rather than what it opened with. */
	s.say("ISUP AD" ADC_EXT_BULLETIN);
	s.say("IBBD BDgeneral PE15 MS262144 TS1786439662 OT0 NP7");

	EXO_ASSERT(s.listener.boards.size() == 1);
	return 1;
});

/*
 * And one it withdrew is a hub that has no boards again: a client must honour what
 * the hub currently claims, not what it claimed when the subscription was made.
 */
EXO_TEST(bbs_client_an_entry_after_the_feature_is_withdrawn_is_ignored,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say(entryLine(TTH_A, "general", 1786436000));
	EXO_ASSERT(s.listener.entries.size() == 1);

	s.say("ISUP RM" ADC_EXT_BULLETIN);
	s.say(entryLine(TTH_B, "general", 1786438120));

	EXO_ASSERT(s.listener.entries.size() == 1);
	return 1;
});

/*
 * What a hostile hub can send and have acted on. Every one of these is dropped by
 * the reader rather than reported as a board with a limit of nothing on it, or as
 * a post named by something that is not a hash.
 */
EXO_TEST(bbs_client_malformed_boards_and_entries_are_dropped,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	/* A board name that is a path, one that is not there at all, and a descriptor
	   with none of the numbers a board is described by. */
	s.say("IBBD BD../../etc/passwd PE15 MS1 TS1 OT0");
	s.say("IBBD PE15 MS1 TS1 OT0");
	s.say("IBBD BDgeneral");

	/* A hash that is not one: lower case, too short, and base32 has no '0'. */
	s.say("IBBL TRkx3tq7zvn5plqgkd3ndbk6ztzg5pyqxsnmfyvjh SI1 BDgeneral ID"
	      + std::string(CID_A) + " TH" + TTH_A + " TS1");
	s.say(std::string("IBBL TRSHORT SI1 BDgeneral ID") + CID_A + " TH" + TTH_A + " TS1");

	/* And an entry describing a post without the fields a post has. */
	s.say(std::string("IBBL TR") + TTH_A + " BDgeneral TS1");

	/* Nothing at all after the fourcc. */
	s.say("IBBD");
	s.say("IBBL");

	EXO_ASSERT(s.listener.boards.empty());
	EXO_ASSERT(s.listener.entries.empty());

	/* Still on the hub, and still listening: none of this is a reason to give up
	   on the connection. */
	s.say(entryLine(TTH_A, "general", 1786436000));
	EXO_ASSERT(s.listener.entries.size() == 1);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* what the client says                                                      */
/* ------------------------------------------------------------------------- */

EXO_TEST(bbs_client_a_subscription_names_the_board_and_the_cursor,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	s.forgetTraffic();

	EXO_ASSERT(s.session->subscribeBoard("general", 1786436000));

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HBBL") == 0);
	EXO_ASSERT(field(line, "BD") == "general");
	EXO_ASSERT(field(line, "TS") == "1786436000");

	/* A request carrying both a hash and a timestamp is malformed, and the two are
	   never written by the same call - a subscription writes the cursor. */
	EXO_ASSERT(!hasField(line, "TR"));
	EXO_ASSERT(!hasField(line, "RM"));

	/* Cancelling is the same command with the flag, and no cursor. */
	EXO_ASSERT(s.session->unsubscribeBoard("general"));
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HBBL") == 0);
	EXO_ASSERT(field(line, "BD") == "general");
	EXO_ASSERT(field(line, "RM") == "1");
	return 1;
});

/*
 * A subscription resumes from the highest timestamp already seen and not from one
 * second later, and the number goes out exactly as it was given.
 *
 * The one thing that must not happen here is the obvious tidiness of advancing the
 * cursor past the last entry received. Timestamps are not unique, so a post the hub
 * accepted in the same second as the last one delivered would be skipped, silently
 * and for ever - the hub replays from the cursor and nothing enters the index
 * behind it. The price of resuming from the second itself is that its entries
 * arrive twice, which a caller keyed on the hash discards.
 */
EXO_TEST(bbs_client_a_resumption_asks_from_the_highest_timestamp_seen,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say(entryLine(TTH_A, "general", 1786436000));
	s.say(entryLine(TTH_B, "general", 1786438120));

	EXO_ASSERT(s.listener.entries.size() == 2);
	if (s.listener.entries.size() != 2) return 0;

	uint64_t highest = 0;
	for (const QuickDC::BoardEntry& entry : s.listener.entries)
		if (entry.timestamp > highest) highest = entry.timestamp;

	EXO_ASSERT(highest == 1786438120);

	s.forgetTraffic();
	EXO_ASSERT(s.session->subscribeBoard("general", highest));

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(field(line, "TS") == "1786438120");

	/* Named as a number rather than compared as one, so that a cursor moved a
	   second on would fail here rather than pass by arithmetic. */
	EXO_ASSERT(field(line, "TS") != "1786438121");

	/* And the whole board is a cursor of zero, written out rather than left to be
	   inferred from an absent field. */
	EXO_ASSERT(s.session->subscribeBoard("general", 0));
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(field(line, "TS") == "0");
	return 1;
});

/*
 * Asking for one entry is a question and not a subscription, and carries the hash
 * instead of a cursor - the two may not both appear.
 */
EXO_TEST(bbs_client_a_single_entry_is_asked_for_by_hash,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	s.forgetTraffic();

	EXO_ASSERT(s.session->requestEntry("general", TTH_A));

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HBBL") == 0);
	EXO_ASSERT(field(line, "BD") == "general");
	EXO_ASSERT(field(line, "TR") == TTH_A);
	EXO_ASSERT(!hasField(line, "TS"));
	EXO_ASSERT(!hasField(line, "RM"));
	return 1;
});

/*
 * A submission carries what the author knows and nothing that is the hub's to say.
 *
 * ID and NI would be this client naming an author; TS would be it choosing its own
 * place in a board's order, which is the order every cursor is made of; TH is not
 * the client's to work out at all. A hub discards them rather than refusing the
 * command, so nothing would go wrong visibly - which is exactly why this is
 * asserted here.
 */
EXO_TEST(bbs_client_a_post_carries_nothing_that_is_the_hubs_to_say,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	s.forgetTraffic();

	EXO_ASSERT(s.session->submitPost(TTH_A, 412, "general", "Hub upgrade on Saturday", 0));

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HBBP") == 0);
	EXO_ASSERT(field(line, "TR") == TTH_A);
	EXO_ASSERT(field(line, "SI") == "412");
	EXO_ASSERT(field(line, "BD") == "general");
	EXO_ASSERT(field(line, "SJ") == "Hub\\supgrade\\son\\sSaturday");

	EXO_ASSERT(!hasField(line, "ID"));
	EXO_ASSERT(!hasField(line, "NI"));
	EXO_ASSERT(!hasField(line, "TH"));
	EXO_ASSERT(!hasField(line, "TS"));

	/* No parent, so it starts a thread - and the thread root is the hub's to
	   derive from that absence. */
	EXO_ASSERT(!hasField(line, "PA"));
	EXO_ASSERT(!hasField(line, "RM"));

	/* A reply names its parent, and nothing else changes. */
	EXO_ASSERT(s.session->submitPost(TTH_B, 298, "general", "Re: Hub upgrade", TTH_A));
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(field(line, "TR") == TTH_B);
	EXO_ASSERT(field(line, "PA") == TTH_A);
	EXO_ASSERT(!hasField(line, "TH"));

	/* A post with no subject leaves the field out rather than sending it empty. */
	EXO_ASSERT(s.session->submitPost(TTH_A, 100, "general", "", 0));
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(!hasField(line, "SJ"));
	return 1;
});

/* Withdrawal is the same command with the flag, naming the entry and the board. */
EXO_TEST(bbs_client_a_withdrawal_names_the_post_and_the_board,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	s.forgetTraffic();

	EXO_ASSERT(s.session->withdrawPost(TTH_A, "general"));

	std::string line;
	EXO_ASSERT(s.hub.nextLine(line));
	EXO_ASSERT(line.compare(0, 4, "HBBP") == 0);
	EXO_ASSERT(field(line, "TR") == TTH_A);
	EXO_ASSERT(field(line, "BD") == "general");
	EXO_ASSERT(field(line, "RM") == "1");

	/* Nothing that describes a post: what is named is an entry the hub holds. */
	EXO_ASSERT(!hasField(line, "SI"));
	EXO_ASSERT(!hasField(line, "SJ"));
	return 1;
});

/*
 * Arguments are checked before anything is written, rather than sent for the hub
 * to refuse. A board name that is a path or a hash that is not one is a fault in
 * this client, and the answer to one of those is not a status message from a
 * stranger describing it.
 */
EXO_TEST(bbs_client_a_malformed_argument_is_refused_here,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());
	s.forgetTraffic();

	/* A name that is not a board name: a path, a space, nothing, and null. */
	EXO_ASSERT(!s.session->subscribeBoard("general/../etc", 0));
	EXO_ASSERT(!s.session->subscribeBoard("two words", 0));
	EXO_ASSERT(!s.session->subscribeBoard("", 0));
	EXO_ASSERT(!s.session->subscribeBoard(0, 0));
	EXO_ASSERT(!s.session->unsubscribeBoard("general\nHBBL"));

	/* And a hash that is not one: lower case, short, and absent. */
	EXO_ASSERT(!s.session->requestEntry("general", "kx3tq7zvn5plqgkd3ndbk6ztzg5pyqxsnmfyvjh"));
	EXO_ASSERT(!s.session->requestEntry("general", "SHORT"));
	EXO_ASSERT(!s.session->requestEntry("general", 0));
	EXO_ASSERT(!s.session->withdrawPost("SHORT", "general"));

	/* A parent that is not a hash is not silently dropped, which would turn a
	   reply into a new thread. */
	EXO_ASSERT(!s.session->submitPost(TTH_A, 412, "general", "a subject", "SHORT"));

	/* A document is a header and the newline that ends it at the very least. */
	EXO_ASSERT(!s.session->submitPost(TTH_A, 0, "general", "a subject", 0));

	/*
	 * And a subject carrying a newline is refused rather than flattened. The
	 * document it belongs to would be refused by every reader, so what is being
	 * submitted is a post nobody could display.
	 */
	EXO_ASSERT(!s.session->submitPost(TTH_A, 412, "general", "one\ntwo", 0));

	pump([] { return false; }, 120);
	s.hub.drain();
	EXO_ASSERT(s.hub.received.empty());
	return 1;
});

/* ------------------------------------------------------------------------- */
/* refusals                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * A refusal of something said about a board is reported as one, with its code and
 * whatever it named, rather than going into the generic error path.
 *
 * 76 is the answer to asking for a post the hub holds no entry for, and is the
 * plainest case of a code this client can act on: the post can stop being asked
 * for. It is also a code that is in no existing switch, so without this it would
 * have come out as an unknown code in the log.
 */
EXO_TEST(bbs_client_a_missing_entry_is_a_board_refusal,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say(std::string("ISTA 1") + std::to_string(ADC_CODE_BULLETIN_NO_ENTRY)
		+ " No\\ssuch\\spost FCBBL TR" + TTH_A);

	EXO_ASSERT(s.listener.refusals.size() == 1);
	if (s.listener.refusals.empty()) return 0;

	EXO_ASSERT(s.listener.refusals[0].code == ADC_CODE_BULLETIN_NO_ENTRY);
	EXO_ASSERT(s.listener.refusals[0].message == "No such post");
	EXO_ASSERT(s.listener.refusals[0].tth == TTH_A);
	EXO_ASSERT(s.listener.refusals[0].board.empty());

	/*
	 * And it is reported once and as one thing. A refusal that reached the interface
	 * twice, or that also arrived as a board this client does not have, would be the
	 * same status understood two ways - which is what the matching by hash exists to
	 * stop. Not checked against the system errors: onStatus() has no path to one, so
	 * an empty collector there is empty whatever this status did.
	 */
	EXO_ASSERT(s.listener.refusals.size() == 1);
	EXO_ASSERT(s.listener.boards.empty());
	EXO_ASSERT(s.listener.entries.empty());
	return 1;
});

/* A board that is not there names itself, so it can be dropped from the interface. */
EXO_TEST(bbs_client_a_missing_board_is_named_in_the_refusal,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say(std::string("ISTA 1") + std::to_string(ADC_CODE_BULLETIN_NO_BOARD)
		+ " No\\ssuch\\sboard FCBBL BDgenrel");

	EXO_ASSERT(s.listener.refusals.size() == 1);
	if (s.listener.refusals.empty()) return 0;

	EXO_ASSERT(s.listener.refusals[0].code == ADC_CODE_BULLETIN_NO_BOARD);
	EXO_ASSERT(s.listener.refusals[0].board == "genrel");
	EXO_ASSERT(s.listener.refusals[0].tth.empty());
	return 1;
});

/*
 * A post the board will not hold is refused against the hash of the post, which is
 * how acceptance and refusal are told apart without a token: one names the hash as
 * an entry, the other names it as an error.
 */
EXO_TEST(bbs_client_a_post_too_large_is_refused_against_its_hash,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say(std::string("ISTA 1") + std::to_string(ADC_CODE_BULLETIN_TOO_LARGE)
		+ " Post\\sexceeds\\s65536\\sbytes FCBBP TR" + TTH_A + " MS65536");

	EXO_ASSERT(s.listener.refusals.size() == 1);
	if (s.listener.refusals.empty()) return 0;

	EXO_ASSERT(s.listener.refusals[0].code == ADC_CODE_BULLETIN_TOO_LARGE);
	EXO_ASSERT(s.listener.refusals[0].message == "Post exceeds 65536 bytes");
	EXO_ASSERT(s.listener.refusals[0].tth == TTH_A);
	return 1;
});

/*
 * A code ADC already had is a board refusal too when FC names one of these
 * commands, and that is the whole reason the flag is read before the switch: a
 * board this session may not post on and a login denied arrive as the same 25.
 */
EXO_TEST(bbs_client_a_denied_permission_on_a_board_is_a_board_refusal,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say(std::string("ISTA 1") + std::to_string(ADC_CODE_ACCESS_DENIED)
		+ " Only\\soperators\\smay\\sstart\\sa\\sthread FCBBP TR" + TTH_A);

	EXO_ASSERT(s.listener.refusals.size() == 1);
	if (s.listener.refusals.empty()) return 0;

	EXO_ASSERT(s.listener.refusals[0].code == ADC_CODE_ACCESS_DENIED);
	EXO_ASSERT(s.listener.refusals[0].tth == TTH_A);

	/* A status naming something else is not a board refusal, however similar the
	   code: what a refusal is about is the command in FC. */
	s.say(std::string("ISTA 1") + std::to_string(ADC_CODE_ACCESS_DENIED)
		+ " Not\\syours\\sto\\ssay FCMSG");
	EXO_ASSERT(s.listener.refusals.size() == 1);
	return 1;
});

/*
 * And what a refusal will not be believed about.
 *
 * A DSTA is another client talking, and no other client is a party to a bulletin
 * board at any point: this extension is spoken between a client and its hub, so a
 * refusal from anywhere else is somebody on the hub inventing an answer to a post
 * they know nothing about. A board name that is not one and a hash that is not one
 * are dropped too, both being remote text bound for an interface.
 */
EXO_TEST(bbs_client_a_refusal_from_another_client_is_not_one,
{
	Session s;
	if (!s.hub.listening) EXO_SKIP("could not listen on a loopback port");
	EXO_ASSERT(s.ready);
	EXO_ASSERT(s.login());

	s.say("BINF BBBB NIalice");
	s.say(std::string("DSTA BBBB AAAA 1") + std::to_string(ADC_CODE_BULLETIN_NO_BOARD)
		+ " No\\ssuch\\sboard FCBBL BDgeneral");

	EXO_ASSERT(s.listener.refusals.empty());

	/* A fatal severity is not one either: none of these is a reason to disconnect
	   a client, so a status that says otherwise is about the connection. */
	s.say(std::string("ISTA 2") + std::to_string(ADC_CODE_BULLETIN_NO_BOARD)
		+ " Go\\saway FCBBL BDgeneral");
	EXO_ASSERT(s.listener.refusals.empty());

	/* The hub itself, with a board name that is a path and a hash that is not a
	   hash: reported, and neither of them passed on. */
	s.say(std::string("ISTA 1") + std::to_string(ADC_CODE_BULLETIN_ERROR)
		+ " Something\\swent\\swrong FCBBP BD../../etc TRnothash");

	EXO_ASSERT(s.listener.refusals.size() == 1);
	if (s.listener.refusals.empty()) return 0;

	EXO_ASSERT(s.listener.refusals[0].code == ADC_CODE_BULLETIN_ERROR);
	EXO_ASSERT(s.listener.refusals[0].board.empty());
	EXO_ASSERT(s.listener.refusals[0].tth.empty());
	EXO_ASSERT(s.listener.refusals[0].message == "Something went wrong");

	/*
	 * And a hub whose words begin with a field name is not read as naming a board:
	 * the message is a sentence a remote party chooses freely, and it comes before
	 * the fields rather than among them.
	 */
	s.say(std::string("ISTA 1") + std::to_string(ADC_CODE_BULLETIN_NO_BOARD)
		+ " BDgeneral\\sis\\sclosed FCBBL");

	EXO_ASSERT(s.listener.refusals.size() == 2);
	if (s.listener.refusals.size() != 2) return 0;

	EXO_ASSERT(s.listener.refusals[1].message == "BDgeneral is closed");
	EXO_ASSERT(s.listener.refusals[1].board.empty());
	return 1;
});
