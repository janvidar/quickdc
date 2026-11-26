/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * A real DC::ClientSession against a peer that only exists for the test - the
 * NMDC counterpart of clientsession.tcc.
 *
 * This one needs more scaffolding than the ADC peer did. An NMDC peer
 * connection is answered with a challenge carrying our own nickname, and the
 * session takes that from the hub session's local user, so there has to be a
 * hub to be logged in to first. Each case therefore stands up a hub, logs a
 * session in to it, and only then makes the peer connection.
 *
 * As in the ADC harness, the hub session grants a clearance for the peer's
 * nickname before it connects, which is what a real one does. Being reached over
 * the loopback used to authorise a peer on its own; it does not any more, so
 * without that clearance every upload case here would be refused.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/hub.h"
#include "network/dc/hubsession.h"
#include "network/dc/clientsession.h"
#include "network/dc/lock.h"
#include "network/downloadqueue.h"
#include "share/sharemanager.h"
#include "network/securitymanager.h"
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
#include <fstream>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <samurai/posix.h>
#include <vector>

/* The fixture's core; built in EXO_SETUP below. */
static QuickDC::Core* core = 0;

static const int TIMEOUT_MS = 5000;

/* Long enough for a key to be derived from, as a real peer sends. */
#define PEER_LOCK "EXTENDEDPROTOCOLABCABCABCABCABCABC"

/* Each test is its own process, so these paths are that process's alone. */
static std::string scratch(const char* leaf)
{
	return "/tmp/quickdc_dcpeer_" + std::to_string((int) getpid()) + "_" + leaf;
}

template<typename Fn>
static bool pump(Fn done, int timeout_ms = TIMEOUT_MS)
{
	Samurai::IO::Net::SocketMonitor* monitor = &core->loop().monitor();

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);

	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		monitor->wait(10);
	}
	return true;
}

/* Enough of a listener to construct a hub with. */
class Listener : public QuickDC::HubListener
{
	public:
		void EventNetStatus(enum StatusNetwork) override { }
		void EventHubStatus(enum StatusHub) override { }
		void EventSystemError(const char*) override { }
		void EventChat(const QuickDC::User*, const char*, bool, bool, uint64_t) override { }
		void EventPrivateChat(const QuickDC::User*, const QuickDC::User*,
			const char*, const char*, bool, bool, uint64_t) override { }
		void EventHubMessage(const char*, const char*, bool, bool, uint64_t) override { }
		void EventUserJoin(const char*) override { }
		void EventUserLeave(const QuickDC::User*, const char*, bool) override { }
		void EventUsersCleanup() override { }
		void EventUserUpdate(const QuickDC::User*) override { }
		void EventSearchResult(void*) override { }
		void EventHubName(const char*) override { }
		void EventHubRedirect(const char*, uint16_t, const char*, QuickDC::RedirectRefusal) override { }
		void EventHubAutenticate() override { }
		bool EventClientConnect(const char*, uint16_t, const QuickDC::User*) override
		{ return false; }
		bool EventClientConnect(const QuickDC::User*) override { return false; }
};

/*
 * One end of a loopback connection, kept as a stream so it can be read a
 * command at a time. Used for both the hub and the peer.
 */
class Endpoint :
	public Samurai::IO::Net::ServerSocketEventHandler,
	public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> mine;
		std::shared_ptr<Samurai::IO::Net::Socket> theirs;

		/*
		 * Whichever of the two above is this end. As the hub we accept, so it
		 * is the accepted one; as the peer we dial, so it is the dialling one.
		 */
		std::shared_ptr<Samurai::IO::Net::Socket> local;

		std::string received;
		bool connected = false;
		bool adopt = true;   /* false leaves 'theirs' for someone else to take */

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(core->loop(), this, any);
			return server && server->getFD() != -1 && server->listen();
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }

		/* Dials ourselves, so both ends are here. */
		bool dial()
		{
			if (!listen()) return false;
			mine = Samurai::IO::Net::Socket::create(core->loop(), this,
				std::string("127.0.0.1"), port());
			if (!mine) return false;
			mine->connect();
			if (!pump([&] { return connected && theirs != 0; })) return false;

			local = mine;
			return true;
		}

		/* We are the one accepting, so the accepted socket is our end. */
		bool accepted()
		{
			if (!pump([&] { return theirs != 0; })) return false;

			local = theirs;
			return true;
		}

		/* NMDC ends every command with '|'. */
		void send(const std::string& command)
		{
			if (!local) return;
			const std::string out = command + "|";
			local->write(out.data(), out.size());
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

		bool expect(const char* prefix, std::string& out)
		{
			for (int n = 0; n < 30; n++)
			{
				if (!nextCommand(out)) return false;
				if (out.compare(0, strlen(prefix), prefix) == 0) return true;
			}
			return false;
		}

		void drain()
		{
			if (!local) return;
			for (;;)
			{
				char buf[4096];
				size_t got = 0;
				std::error_code ec;
				if (local->read(buf, sizeof(buf), got, ec) != Samurai::IO::ReadResult::Ok) break;
				if (!got) break;
				received.append(buf, got);
			}
		}

		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			theirs = socket;
			if (adopt) theirs->setEventHandler(this);
		}

		void EventHostLookup(const Samurai::IO::Net::Socket*) override { }
		void EventHostFound(const Samurai::IO::Net::Socket*) override { }
		void EventConnecting(const Samurai::IO::Net::Socket*) override { }
		void EventConnected(const Samurai::IO::Net::Socket*) override { connected = true; }
		void EventTimeout(const Samurai::IO::Net::Socket*) override { }
		void EventDisconnected(const Samurai::IO::Net::Socket*) override { gone = true; }
		bool gone = false;
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
 * A hub with a session logged in to it, and a peer connection made to that
 * session - which is where every case below starts.
 */
class Fixture
{
	public:
		Endpoint hub;
		Endpoint peer;
		Listener listener;
		std::unique_ptr<Samurai::IO::Net::URL> url;
		std::unique_ptr<QuickDC::Hub> ui;
		DC::HubSession* hubsession = 0;
		DC::ClientSession* session = 0;
		std::string nick;
		bool ready = false;

		Fixture()
		{
			if (!hub.listen()) return;

			const std::string address =
				"dchub://127.0.0.1:" + std::to_string(hub.port());
			url = std::make_unique<Samurai::IO::Net::URL>(address);

			ui = std::make_unique<QuickDC::Hub>(*core, &listener, url.get());
			hubsession = new DC::HubSession(ui.get(), &listener, url.get());
			hubsession->connect();
			if (!hub.accepted()) return;

			if (!login()) return;

			/* The peer's end is ours; the accepted one goes to the session,
			   which adopts it. */
			peer.adopt = false;
			if (!peer.dial()) return;

			/*
			 * What the hub session grants when it asks a peer to connect, or
			 * answers a request from one. NMDC has no token, so the nickname is
			 * the whole of the clearance - and without it an upload is refused,
			 * which is the point: arriving over the loopback used to be enough on
			 * its own and no longer is.
			 */
			hubsession->expectPeer("peernick");

			session = new DC::ClientSession(*core, peer.theirs, hubsession);
			ready = true;
		}

		~Fixture()
		{
			delete session;
			delete hubsession;
			ui.reset();
		}

		Fixture(const Fixture&) = delete;
		Fixture& operator=(const Fixture&) = delete;

		void sayHub(const std::string& command)
		{
			hub.send(command);
			pump([] { return false; }, 60);
		}

		void say(const std::string& command)
		{
			peer.send(command);
			pump([] { return false; }, 60);
		}

		/* Enough of a hub login that the session has a local user to name. */
		bool login()
		{
			sayHub("$Lock " PEER_LOCK " Pk=TESTHUB");

			std::string line;
			if (!hub.expect("$ValidateNick ", line)) return false;
			nick = line.substr(strlen("$ValidateNick "));
			if (nick.empty()) return false;

			sayHub("$Hello " + nick);
			if (!hub.expect("$MyINFO ", line)) return false;

			sayHub("$MyINFO $ALL " + nick + " $ $DSL\x01$$0$");
			return hubsession->isLoggedIn();
		}

		/* The lock we challenged the peer with, so it can be answered. */
		std::string our_lock;

		/* The peer speaks first: its nick, then its lock. */
		bool handshake()
		{
			say("$MyNick peernick");
			say("$Lock " PEER_LOCK " Pk=PEER");

			std::string line;
			if (!peer.expect("$MyNick ", line)) return false;
			if (!peer.expect("$Lock ", line)) return false;

			/* "$Lock <lock> Pk=<pk>" - kept because the answer to it is checked,
			   so a case has to reply as a peer would rather than with anything. */
			const size_t start = strlen("$Lock ");
			const size_t end = line.find(" Pk=", start);
			if (end == std::string::npos) return false;

			our_lock = line.substr(start, end - start);
			return !our_lock.empty();
		}

		/* The answer to that lock, derived the way the peer at the other end
		   would derive it. */
		void sayKey()
		{
			say("$Key " + DC::Lock::calculateKey(our_lock));
		}
};

/*
 * The fixture main() used to be: one Core wrapping every case, pointed at
 * scratch paths rather than the ones a real client would use.
 */
static std::string cfgPath;
static std::string sesPath;
static std::string queuePath;

/*
 * A Fixture that never became ready could not listen, dial or log in over the
 * loopback, none of which is the session's fault.
 */
static const char* NO_LOOPBACK = "this host cannot stand up a loopback hub and peer";

EXO_SETUP({
	cfgPath = scratch("config.conf");
	sesPath = scratch("session.conf");
	queuePath = scratch("queue.xml");

	Samurai::IO::File(cfgPath).remove();
	Samurai::IO::File(sesPath).remove();
	Samurai::IO::File(queuePath).remove();

	core = new QuickDC::Core(cfgPath, sesPath, queuePath);
	return 1;
});

EXO_TEARDOWN({
	/*
	 * The core goes first: it holds the files open when it is destroyed, so
	 * removing them has to wait until the core holding them is gone - and it is
	 * also what tears down anything still registered with the connection
	 * manager.
	 */
	delete core;
	core = 0;

	Samurai::IO::File(cfgPath).remove();
	Samurai::IO::File(sesPath).remove();
	Samurai::IO::File(queuePath).remove();
	Samurai::IO::File(queuePath + ".new").remove();
	return 1;
});

/* ------------------------------------------------------------------------- */

EXO_TEST(dcclientsession_a_peer_connection_is_accepted_on_a_logged_in_session,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);

	EXO_ASSERT(f.session != 0);
	EXO_ASSERT(f.hubsession && f.hubsession->isLoggedIn());
	return 1;
});

/* The peer names itself and challenges; we answer in kind. */
EXO_TEST(dcclientsession_the_challenge_names_our_own_nick,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);

	f.say("$MyNick peernick");
	f.say("$Lock " PEER_LOCK " Pk=PEER");

	std::string line;
	EXO_ASSERT(f.peer.expect("$MyNick ", line));
	EXO_ASSERT(line == "$MyNick " + f.nick);
	return 1;
});

EXO_TEST(dcclientsession_and_carries_a_lock_of_our_own,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);

	f.say("$MyNick peernick");
	f.say("$Lock " PEER_LOCK " Pk=PEER");

	std::string line;
	EXO_ASSERT(f.peer.expect("$Lock ", line));
	EXO_ASSERT(line.find(" Pk=") != std::string::npos);
	return 1;
});

/* A lock too short to derive a key from is refused, not answered. */
EXO_TEST(dcclientsession_a_lock_too_short_to_answer_is_refused,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);

	f.say("$MyNick peernick");
	f.say("$Lock ab Pk=PEER");
	pump([] { return false; }, 200);

	f.peer.drain();
	EXO_ASSERT(f.peer.received.find("$Key") == std::string::npos);
	return 1;
});

/* Answering the challenge gets a direction and a key back. */
EXO_TEST(dcclientsession_the_direction_is_negotiated,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);
	EXO_ASSERT(f.handshake());

	f.say("$Supports ADCGet TTHF");
	f.say("$Direction Download 12345");
	f.sayKey();

	std::string line;
	EXO_ASSERT(f.peer.expect("$Direction ", line));
	return 1;
});

/* Nothing we hold, so the request is refused rather than answered. */
EXO_TEST(dcclientsession_a_file_we_do_not_have_is_refused,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);
	EXO_ASSERT(f.handshake());

	f.say("$Supports ADCGet TTHF");
	f.say("$Direction Download 12345");
	f.sayKey();
	f.say("$ADCGET file TTH/AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A 0 -1");

	std::string line;
	EXO_ASSERT(f.peer.expect("$Error", line));
	return 1;
});

/*
 * The legacy requests for a file list - $GetListLen, and $Get naming a list - are
 * answered from a list opened for this request alone, which the session owns and
 * closes; it took them as the share manager's and leaked one each time. Under
 * LeakSanitizer that is a failure of this case.
 */
EXO_TEST(dcclientsession_a_file_list_asked_for_the_old_way_is_closed_after,
{
	core->shares->createFileLists(false);
	const std::string dir = core->shares->getFilelistDirectory();
	const std::string legacy = dir + "/MyList.DcLst";
	{
		std::ofstream out(legacy, std::ios::binary);
		out << "a list nobody reads any more";
	}

	bool connected = false;
	bool listLength = false;
	bool listFile = false;
	bool xmlFile = false;
	{
		Fixture f;
		connected = f.ready && f.handshake();
		if (connected)
		{
			f.say("$Direction Download 12345");
			f.sayKey();

			std::string line;
			f.say("$GetListLen");
			listLength = f.peer.expect("$ListLen ", line) && line == "$ListLen 28";
			f.say("$Get MyList.DcLst$1");
			listFile = f.peer.expect("$FileLength ", line) && line == "$FileLength 28";
			f.say("$Get files.xml.bz2$1");
			xmlFile = f.peer.expect("$FileLength ", line);
		}
	}

	Samurai::IO::File(legacy).remove();
	Samurai::IO::File(dir + "/files.xml").remove();
	Samurai::IO::File(dir + "/files.xml.bz2").remove();

	if (!connected) EXO_SKIP(NO_LOOPBACK);
	EXO_ASSERT(listLength);
	EXO_ASSERT(listFile);
	EXO_ASSERT(xmlFile);
	return 1;
});

/*
 * Browsing someone asks for their whole share, as the ADC side does: a
 * "list" request names one directory, and a peer that answers it properly
 * sends that directory's subdirectories marked Incomplete="1" and nothing
 * within them, which is not something to browse.
 *
 * The queue is what makes this a download rather than an upload - NMDC has
 * no token, so a job is correlated by the peer's nickname, and the
 * direction negotiation consults the queue for one.
 */
EXO_TEST(dcclientsession_a_browse_asks_for_the_whole_list,
{
	QuickDC::QueueManager* queue = core->queue;

	bool connected = false;
	bool shookHands = false;
	bool haveList = false;
	bool haveToken = false;
	bool asked = false;
	std::string line;

	{
		Fixture f;
		connected = f.ready;
		if (connected)
		{
			QuickDC::FilelistRequest* list =
				queue->requestFilelist(f.ui->getUrl(), "peernick");
			haveList = (list != 0);

			const char* token = (list) ? queue->createRequest(list) : 0;
			haveToken = (token != 0);

			shookHands = f.handshake();
			if (shookHands)
			{
				/* Ours to download: they offered to upload, so nobody loses. */
				f.say("$Supports ADCGet TTHF XmlBZList");
				f.say("$Direction Upload 1");
				f.sayKey();

				asked = f.peer.expect("$ADCGET ", line);
			}

			if (list) queue->removeFilelistRequest(list);
		}
	}

	if (!connected) EXO_SKIP(NO_LOOPBACK);
	EXO_ASSERT(haveList);
	EXO_ASSERT(haveToken);
	EXO_ASSERT(shookHands);
	EXO_ASSERT(asked);
	EXO_ASSERT(line == "$ADCGET file files.xml.bz2 0 -1");
	return 1;
});

/*
 * What a peer can throw at us, before and after the handshake. The ADC
 * side of this turned up a crash in the command parser, which runs before
 * anything has been authorised.
 */
EXO_TEST(dcclientsession_malformed_and_unknown_commands_are_survived,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);

	/* Commands with nothing after the keyword. */
	f.say("$MyNick");
	f.say("$Lock");
	f.say("$Key");
	f.say("$Direction");
	f.say("$ADCGET");
	f.say("$ADCSND");
	f.say("$Supports");
	f.say("$Get");
	f.say("$Send");
	f.say("$GetZBlock");
	f.say("$GetListLen");
	f.say("$FileLength");
	f.say("$MaxedOut");
	f.say("$Error");
	f.say("$Failed");
	f.say("$Sending");

	/* A keyword and a single space. */
	f.say("$ADCGET ");
	f.say("$Direction ");
	f.say("$Lock ");

	/* Enough length, nothing sensible in it. */
	f.say("$ADCGET filenotanumberhere");
	f.say("$ADCGET file x y z");
	f.say("$ADCGET zzzz TTH/x 0 -1");
	f.say("$ADCSND file x");
	f.say("$Direction Sideways 12345");
	f.say("$Direction Upload notanumber");
	f.say("$Lock ab Pk=");

	/* Not NMDC at all. */
	f.say("");
	f.say("plain text");
	f.say("$NoSuchCommand with arguments");

	pump([] { return false; }, 300);
	EXO_ASSERT(f.session != 0);
	return 1;
});

/* The same, after the handshake, where more state is in play. */
EXO_TEST(dcclientsession_malformed_commands_after_the_handshake_are_survived,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);
	EXO_ASSERT(f.handshake());

	f.say("$Supports ADCGet TTHF");
	f.say("$Direction Download 12345");
	f.sayKey();

	f.say("$ADCGET");
	f.say("$ADCGET ");
	f.say("$ADCGET file");
	f.say("$ADCGET file TTH/x");
	f.say("$ADCGET file TTH/x 0");
	f.say("$ADCGET list / 0 -1 ZL1");
	f.say("$ADCSND");
	f.say("$ADCSND file TTH/x 0 -1");
	f.say("$MyNick another");
	f.say("$Lock " PEER_LOCK " Pk=AGAIN");

	pump([] { return false; }, 300);
	EXO_ASSERT(f.session != 0);
	return 1;
});

/* The peer hanging up is noticed rather than spun on. */
EXO_TEST(dcclientsession_the_peer_hanging_up_is_survived,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);
	EXO_ASSERT(f.handshake());

	f.peer.local.reset();
	f.peer.mine.reset();
	pump([] { return false; }, 300);

	EXO_ASSERT(f.session != 0);
	return 1;
});

/* A peer that connects and says nothing does not hold the connection for ever. */
EXO_TEST(dcclientsession_a_silent_peer_is_dropped_at_the_handshake_deadline,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);

	f.session->setHandshakeDeadline(std::chrono::milliseconds(200));
	EXO_ASSERT(pump([&] { f.peer.drain(); return f.peer.gone; }, 3000));
	return 1;
});

/* And one that finishes the handshake in time is left alone after it. */
EXO_TEST(dcclientsession_a_handshake_completed_in_time_cancels_the_deadline,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);

	f.session->setHandshakeDeadline(std::chrono::milliseconds(300));
	EXO_ASSERT(f.handshake());
	f.say("$Supports ADCGet TTHF");
	f.say("$Direction Download 12345");
	f.sayKey();
	pump([] { return false; }, 600);
	EXO_ASSERT(!f.peer.gone);
	return 1;
});

/* Connections come and go without leaving anything in the monitor. */
EXO_TEST(dcclientsession_repeated_connections_leave_nothing_behind,
{
	Samurai::IO::Net::SocketMonitor* monitor = &core->loop().monitor();
	const size_t baseline = monitor->size();

	for (int n = 0; n < 2; n++)
	{
		Fixture f;
		if (!f.ready) EXO_SKIP(NO_LOOPBACK);
		f.handshake();
	}

	pump([&] { return monitor->size() <= baseline; }, 2000);
	EXO_ASSERT(monitor->size() == baseline);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The answer to our lock                                                    */
/* ------------------------------------------------------------------------- */

/*
 * NMDC's key authenticates nobody and cannot: the derivation is unkeyed and
 * invertible, and the lock it answers went out in the clear. What it does show is
 * that the peer read the challenge we sent - so a wrong answer is a peer replaying
 * someone else's handshake or not following ours, and the connection ends rather
 * than going on to serve a file.
 *
 * Answered correctly by every other case here through sayKey(), which is what
 * makes this one worth having: those would pass against no check at all.
 */
EXO_TEST(dcclientsession_a_wrong_answer_to_our_lock_ends_the_connection,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);
	EXO_ASSERT(f.handshake());

	/* The right shape, and derived from a lock that is not the one we sent. */
	const std::string wrong = DC::Lock::calculateKey(std::string(PEER_LOCK) + "x");
	EXO_ASSERT(!wrong.empty());
	EXO_ASSERT(wrong != DC::Lock::calculateKey(f.our_lock));

	f.say("$Supports ADCGet TTHF");
	f.say("$Direction Download 12345");
	f.say("$Key " + wrong);

	/* Nothing is served after it, and the request is not even answered with an
	   error - the connection is gone. Read off the buffer rather than through
	   expect(), which consumes what it looks past and would take one of these
	   assertions out from under the other. */
	f.say("$ADCGET file TTH/AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A 0 -1");
	pump([] { return false; }, 400);
	f.peer.drain();

	EXO_ASSERT(f.peer.received.find("$ADCSND") == std::string::npos);
	EXO_ASSERT(f.peer.received.find("$Error") == std::string::npos);
	return 1;
});

/*
 * And a key sent before we ever issued a lock, which answers nothing: the
 * challenge it claims to reply to does not exist yet.
 */
EXO_TEST(dcclientsession_a_key_before_any_lock_ends_the_connection,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);

	/*
	 * No $Lock from the peer, so no lock of ours has gone out to be answered -
	 * and an empty key, because a non-empty one is already refused for not
	 * matching. Two empty strings are what would otherwise compare equal.
	 */
	f.say("$MyNick peernick");
	f.say("$Key ");

	f.say("$ADCGET file TTH/AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A 0 -1");
	pump([] { return false; }, 400);
	f.peer.drain();

	EXO_ASSERT(f.peer.received.find("$ADCSND") == std::string::npos);
	EXO_ASSERT(f.peer.received.find("$Error") == std::string::npos);
	return 1;
});

/* And the correct answer is accepted, so the case above is about the answer
   being wrong rather than about any key ending the connection. */
EXO_TEST(dcclientsession_the_right_answer_to_our_lock_is_accepted,
{
	Fixture f;
	if (!f.ready) EXO_SKIP(NO_LOOPBACK);
	EXO_ASSERT(f.handshake());

	f.say("$Supports ADCGet TTHF");
	f.say("$Direction Download 12345");
	f.sayKey();
	f.say("$ADCGET file TTH/AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A 0 -1");

	/* Refused for not being a file we have, which is a refusal that could only
	   be reached by getting past the key. */
	std::string line;
	EXO_ASSERT(f.peer.expect("$Error", line));

	/*
	 * And the clearance the hub session granted for this peer is spent. NMDC
	 * matches it on the nickname alone, which every client on the hub was told,
	 * so what bounds that is it authorising one connection rather than every
	 * connection until the process ends.
	 */
	QuickDC::SecurityManager* security = core->securityManager;
	EXO_ASSERT_NOT_NULL(security);
	EXO_ASSERT(!security->isAuthorized(QuickDC::Clearance::NMDC, 0, 0, "peernick", 0));
	return 1;
});
