/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * A real ADC::ClientSession against a peer that only exists for the test.
 *
 * This is the connection two clients make directly to each other to move a
 * file, rather than the one either makes to a hub. The session is handed an
 * accepted socket, which is the inbound case: the peer dialled us, so it
 * speaks first and we answer.
 *
 * A session that carries a token also gets a clearance granted for it, which is
 * what a hub session does when it asks a peer to connect. Without one an upload
 * is refused, and that is the point: being reached over the loopback used to
 * authorise a peer on its own, and no longer does.
 */

#include "quickdc.h"
#include "api/core.h"
#include "network/adc/adccommon.h"
#include "network/adc/client/adcclientsession.h"
#include "network/downloadqueue.h"
#include "network/hubsession.h"
#include "network/securitymanager.h"

#include <samurai/io/file.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/inetaddress.h>

#include <chrono>
#include <errno.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>

static const int TIMEOUT_MS = 5000;

/* 39 base32 characters, which is all CID::verify() asks for. */
#define PEER_CID "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define A_TTH    "AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"

/* Each test is its own process, so these paths are that process's alone. */
static std::string scratch(const char* leaf)
{
	return "/tmp/quickdc_peertest_" + std::to_string((int) getpid()) + "_" + leaf;
}

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

/*
 * The peer's end of the connection. Listens, dials itself, and hands the
 * accepted socket to the session under test while keeping the other end.
 */
class Peer :
	public Samurai::IO::Net::ServerSocketEventHandler,
	public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		std::shared_ptr<Samurai::IO::Net::Socket> mine;     /* the peer's end */
		std::shared_ptr<Samurai::IO::Net::Socket> theirs;   /* the session's */
		std::string received;
		bool connected = false;

		bool connect()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(this, any);
			if (!server || server->getFD() == -1 || !server->listen()) return false;

			mine = Samurai::IO::Net::Socket::create(this,
				std::string("127.0.0.1"), server->getLocalPort());
			if (!mine) return false;
			mine->connect();

			return pump([&] { return connected && theirs != 0; });
		}

		/* ADC ends every command with a newline. */
		void send(const std::string& command)
		{
			if (!mine) return;
			const std::string out = command + "\n";
			mine->write(out.data(), out.size());
		}

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

		bool expect(const char* prefix, std::string& out)
		{
			for (int n = 0; n < 20; n++)
			{
				if (!nextLine(out)) return false;
				if (out.compare(0, strlen(prefix), prefix) == 0) return true;
			}
			return false;
		}

		void drain()
		{
			if (!mine) return;
			for (;;)
			{
				char buf[4096];
				size_t got = 0;
				std::error_code ec;
				if (mine->read(buf, sizeof(buf), got, ec) != Samurai::IO::ReadResult::Ok) break;
				if (!got) break;
				received.append(buf, got);
			}
		}

		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			/* Left unadopted: the session adopts it, which is what registers
			   it with the monitor. */
			theirs = socket;
		}

		void EventHostLookup(const Samurai::IO::Net::Socket*) override { }
		void EventHostFound(const Samurai::IO::Net::Socket*) override { }
		void EventConnecting(const Samurai::IO::Net::Socket*) override { }
		void EventConnected(const Samurai::IO::Net::Socket*) override { connected = true; }
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

/*
 * The hub the session would relay a peer error through, and nothing else it does.
 *
 * A DSTA is only ever sent by way of the hub session, so this is where what
 * reaches the other peer can be read off. Everything else is a stub: the case
 * below drives one call and looks at its arguments.
 */
class RecordingHubSession : public QuickDC::HubSession
{
	public:
		RecordingHubSession() : QuickDC::HubSession(0) { }

		size_t errors = 0;
		std::string sid;
		std::string code;
		std::string message;
		std::string token;

		void sendPeerError(const char* remote_id, const char* code_, const char* msg,
			const char* token_, const char*) override
		{
			errors++;
			sid = remote_id ? remote_id : "";
			code = code_ ? code_ : "";
			message = msg ? msg : "";
			token = token_ ? token_ : "";
		}

		void connect() override { }
		void disconnect() override { }
		void sendPrivateChatMessage(const QuickDC::User*, const char*, bool) override { }
		void sendChatMessage(const char*, bool) override { }
		void sendAdminUserKick(const QuickDC::User*) override { }
		void sendAdminUserRedirect(const QuickDC::User*, const char*, const char*) override { }
		void sendConnectionRequest(const QuickDC::User*, const char*) override { }
		void sendSearchRequest(QuickDC::Share::SearchRequest*) override { }
		void sendClientInfo() override { }
		void sendPassword(const char*) override { }
		const QuickDC::User* getUser(const char*) override { return 0; }
		const QuickDC::User* getLocalUser() override { return 0; }
		QuickDC::UserManager* getUserManager() override { return 0; }
		void onSearchResult(QuickDC::Share::SearchResult*) override { }
		bool isLoggedIn() const override { return true; }
		bool isSecure() const override { return false; }
};

/*
 * The session, with the socket error handler reachable.
 *
 * The socket monitor is the only other caller of it, and no case can make the
 * kernel report a chosen errno on demand - so it is called with exactly what
 * samurai would have handed it.
 */
class ErrorableSession : public ADC::ClientSession
{
	public:
		using ADC::ClientSession::ClientSession;
		using ADC::ClientSession::EventError;
};

/* A connected pair with the session on the accepted end. */
class Session
{
	public:
		Peer peer;
		ADC::ClientSession* session = 0;
		bool ready = false;

		explicit Session(const char* token = 0)
		{
			if (!peer.connect()) return;

			/* What a hub session grants when it asks this peer to connect, and
			   what the session then checks the peer's CINF against. */
			if (token)
			{
				granted_token = token;
				QuickDC::Core::getInstance()->securityManager->grant(
					new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
						QuickDC::Clearance::ADC, 0, PEER_CID, token));
			}

			session = new ADC::ClientSession(peer.theirs, 0, token, 0, false, 0);
			ready = true;
		}

		~Session()
		{
			delete session;

			/*
			 * By value, not by pointer: the session consumes the clearance when it
			 * reads the peer's CINF, and that path deletes it - so holding the
			 * pointer and deleting it here was a double free. This overload deletes
			 * it if it is still listed and does nothing if it is not, which covers
			 * both the cases that got that far and the ones that did not.
			 */
			if (!granted_token.empty())
			{
				QuickDC::Core::getInstance()->securityManager->revoke(
					QuickDC::Clearance::ADC, 0, PEER_CID, granted_token.c_str());
			}
		}

		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		void say(const std::string& command)
		{
			peer.send(command);
			pump([] { return false; }, 60);
		}

	private:
		std::string granted_token;

	public:
		/* The peer speaks first on an inbound connection. */
		bool handshake()
		{
			say("CSUP ADBASE ADTIGR");

			std::string line;
			if (!peer.expect("CSUP ", line)) return false;
			return peer.expect("CINF ", line);
		}
};

/*
 * The fixture main() used to be: one Core wrapping every case, pointed at
 * scratch paths rather than the ones a real client would use.
 */
static std::string cfgPath;
static std::string sesPath;
static std::string queuePath;
static QuickDC::Core* core = 0;

/* Nothing to run against without one, so every case starts here. */
static const char* CANNOT_CONNECT = "this host cannot make a loopback connection";

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
	 * removing them has to wait until the core holding them is gone. It is also
	 * what tears down any session still registered with the connection manager,
	 * which is what the leaked-session case below is here to exercise.
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

EXO_TEST(clientsession_a_peer_connection_is_accepted,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	EXO_ASSERT(s.session != 0);
	return 1;
});

/* The peer opens with its support list, and we answer with ours. */
EXO_TEST(clientsession_csup_is_answered_with_csup,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	s.say("CSUP ADBASE ADTIGR");

	std::string line;
	EXO_ASSERT(s.peer.expect("CSUP ", line));
	EXO_ASSERT(line.find("ADBASE") != std::string::npos);
	return 1;
});

/* And with who we are. */
EXO_TEST(clientsession_and_with_cinf,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	s.say("CSUP ADBASE ADTIGR");

	std::string line;
	EXO_ASSERT(s.peer.expect("CINF ", line));
	EXO_ASSERT(line.find("ID") != std::string::npos);
	EXO_ASSERT(line.find("VE") != std::string::npos);
	return 1;
});

/* A connection made for a job quotes that job's token back. */
EXO_TEST(clientsession_a_token_is_quoted_back_in_our_cinf,
{
	Session s("thetoken");
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	s.say("CSUP ADBASE");

	std::string line;
	EXO_ASSERT(s.peer.expect("CINF ", line));
	EXO_ASSERT(line.find("TOthetoken") != std::string::npos);
	return 1;
});

/* Without one, there is nothing to quote. */
EXO_TEST(clientsession_no_token_means_none_is_quoted,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	s.say("CSUP ADBASE");

	std::string line;
	EXO_ASSERT(s.peer.expect("CINF ", line));
	EXO_ASSERT(line.find(" TO") == std::string::npos);
	return 1;
});

/*
 * Asking for a file before saying who you are gets nothing. This is the
 * one authorisation assertion the loopback shortcut does not decide:
 * upload_approved is only ever set from CINF, which has not arrived.
 */
EXO_TEST(clientsession_a_file_asked_for_before_cinf_is_not_sent,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	s.say("CSUP ADBASE");

	std::string line;
	EXO_ASSERT(s.peer.expect("CINF ", line));

	s.say("CGET file TTH/" A_TTH " 0 -1");
	pump([] { return false; }, 200);

	s.peer.drain();
	EXO_ASSERT(s.peer.received.find("CSND") == std::string::npos);
	return 1;
});

/* A CINF whose CID is not a CID is not accepted as one. */
EXO_TEST(clientsession_a_malformed_cid_does_not_identify_a_peer,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	s.say("CSUP ADBASE");

	std::string line;
	EXO_ASSERT(s.peer.expect("CINF ", line));

	s.say("CINF IDnotacid");
	s.say("CGET file TTH/" A_TTH " 0 -1");
	pump([] { return false; }, 200);

	s.peer.drain();
	EXO_ASSERT(s.peer.received.find("CSND") == std::string::npos);
	return 1;
});

/*
 * An upload is only ever considered once some token names a job, so this one and
 * the unknown-type case below carry one - and the fixture grants the clearance
 * that goes with it.
 */
EXO_TEST(clientsession_a_file_we_do_not_have_is_refused,
{
	Session s("job1");
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);
	EXO_ASSERT(s.handshake());

	s.say("CINF ID" PEER_CID);
	s.say("CGET file TTH/" A_TTH " 0 -1");

	std::string line;
	EXO_ASSERT(s.peer.expect("CSTA ", line));
	return 1;
});

/*
 * Browsing someone asks for their whole share.
 *
 * A "list" request names a single directory, and a peer that implements it
 * properly answers with that directory's subdirectories marked
 * Incomplete="1" and nothing else - one folder and no way into it. The
 * share entire is the rootless "files.xml.bz2" in the file namespace, so
 * that is what a browse asks for.
 */
EXO_TEST(clientsession_a_browse_asks_for_the_whole_list,
{
	QuickDC::QueueManager* queue = QuickDC::Core::getInstance()->queue;

	QuickDC::FilelistRequest* list =
		queue->requestFilelist("adc://127.0.0.1:1511", PEER_CID);
	EXO_ASSERT(list != 0);

	/* The token is what ties the connection to the queued browse. */
	const char* token = queue->createRequest(list);
	EXO_ASSERT(token != 0);

	bool connected = false;
	bool shookHands = false;
	bool asked = false;
	std::string line;

	{
		Session s(token);
		connected = s.ready;
		if (connected)
		{
			shookHands = s.handshake();
			if (shookHands)
			{
				s.say("CINF ID" PEER_CID);
				asked = s.peer.expect("CGET ", line);
			}
		}
	}

	/* The session is gone, so the queued browse can be taken away with it. */
	queue->removeFilelistRequest(list);

	if (!connected) EXO_SKIP(CANNOT_CONNECT);
	EXO_ASSERT(shookHands);
	EXO_ASSERT(asked);
	EXO_ASSERT(line == "CGET file files.xml.bz2 0 -1");
	return 1;
});

/* A well formed GET for a type nobody defined is refused, not acted on. */
EXO_TEST(clientsession_an_unknown_get_type_is_refused,
{
	Session s("job1");
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);
	EXO_ASSERT(s.handshake());

	s.say("CINF ID" PEER_CID);
	s.say("CGET junk TTH/" A_TTH " 0 -1");

	std::string line;
	EXO_ASSERT(s.peer.expect("CSTA ", line));
	return 1;
});

/*
 * A type that is not four characters is refused by the parser, before the
 * session sees it - the type is read four characters deep whatever its
 * length.
 */
EXO_TEST(clientsession_a_type_that_is_not_four_characters_is_refused,
{
	Session s("job1");
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);
	EXO_ASSERT(s.handshake());

	s.say("CINF ID" PEER_CID);
	s.say("CGET ab TTH/" A_TTH " 0 -1");
	s.say("CGET toolongatype TTH/" A_TTH " 0 -1");
	pump([] { return false; }, 200);

	s.peer.drain();
	EXO_ASSERT(s.peer.received.find("CSND") == std::string::npos);
	return 1;
});

/* Without a job named by either side there is nothing to approve. */
EXO_TEST(clientsession_a_connection_for_no_job_uploads_nothing,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);
	EXO_ASSERT(s.handshake());

	s.say("CINF ID" PEER_CID);
	s.say("CGET file TTH/" A_TTH " 0 -1");
	pump([] { return false; }, 300);

	s.peer.drain();
	EXO_ASSERT(s.peer.received.find("CSND") == std::string::npos);
	EXO_ASSERT(s.peer.received.find("CSTA") == std::string::npos);
	return 1;
});

/* A status from the peer is read rather than answered in kind. */
EXO_TEST(clientsession_a_status_from_the_peer_is_survived,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);
	EXO_ASSERT(s.handshake());

	s.say("CINF ID" PEER_CID);
	s.say("CSTA 151 File\\snot\\savailable");
	pump([] { return false; }, 200);

	EXO_ASSERT(s.session != 0);
	return 1;
});

/* What a peer can throw at us. */
EXO_TEST(clientsession_malformed_and_unknown_commands_are_survived,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);
	EXO_ASSERT(s.handshake());

	/* Commands with nothing after the fourcc. */
	s.say("CINF");
	s.say("CGET");
	s.say("CSND");
	s.say("CSUP");
	s.say("CSTA");

	/* Enough arguments, none of them sensible. */
	s.say("CGET file  x y");
	s.say("CSND file TTH/short 0 -1");
	s.say("CGET file TTH/" A_TTH " notanumber -1");

	/* Not ADC at all. */
	s.say("");
	s.say("not a command");
	s.say("ZZZZ argument");

	pump([] { return false; }, 200);
	EXO_ASSERT(s.session != 0);
	return 1;
});

/*
 * A connection that fails, and what the peer that asked for it is told.
 *
 * samurai reports a socket error with strerror() of the local errno, and that
 * text went out in the DSTA the hub relays to the peer - telling it what this
 * host's operating system is and what state it is in. The status code and the
 * token are what the peer needs, so those stay; the phrase is ours now, and the
 * errno is logged and goes nowhere else.
 */
EXO_TEST(clientsession_a_socket_error_is_not_repeated_to_the_peer,
{
	Peer peer;
	if (!peer.connect()) EXO_SKIP(CANNOT_CONNECT);

	ErrorableSession session(peer.theirs, 0, "job1", 0, false, 0);

	RecordingHubSession hub;
	session.setHubSession(&hub);

	/* The C library's text for a refused connection, which samurai passes on. */
	const std::string errno_text = strerror(ECONNREFUSED);
	EXO_ASSERT(!errno_text.empty());

	session.EventError(peer.theirs.get(),
		Samurai::IO::Net::SocketError::ConnectionRefused, errno_text.c_str());

	/* The peer is told, because it is waiting for a connection that is not
	   coming, and the code and the token are what say which one. */
	EXO_ASSERT(hub.errors == 1);
	EXO_ASSERT(hub.code == "242");
	EXO_ASSERT(hub.token == "job1");

	if (hub.message.find(errno_text) != std::string::npos)
	{
		const std::string why = "our errno text was sent to the peer: '"
			+ hub.message + "'";
		EXO_FAIL(why.c_str());
	}

	EXO_ASSERT(hub.message == ADC_STR_CONNECTION_FAILED);
	return 1;
});

/*
 * And a connection that was made and then ended is not reported as one that could
 * not be made.
 *
 * The code above says a direct connection failed, which is a thing that can only be
 * true while one is being established. Once both peers have named themselves the
 * connection exists, and how it ends afterwards is not news the other end needs from
 * us: a peer that closes as soon as it has what it asked for ends the stream, and on
 * some systems ends it with a reset that arrives here as a read error like any other.
 *
 * Sent anyway it is worse than noise. It reaches the peer through the hub as a fatal
 * status against the token of the transfer that has just finished - which tells it the
 * download it completed failed - and every successful upload ended with one.
 */
EXO_TEST(clientsession_an_error_after_the_connection_was_made_is_not_reported_to_the_peer,
{
	Peer peer;
	if (!peer.connect()) EXO_SKIP(CANNOT_CONNECT);

	/* What the hub granted when it asked this peer to connect; the CINF spends it. */
	QuickDC::Core::getInstance()->securityManager->grant(
		new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
			QuickDC::Clearance::ADC, 0, PEER_CID, "job9"));

	ErrorableSession session(peer.theirs, 0, "job9", 0, false, 0);

	RecordingHubSession hub;
	session.setHubSession(&hub);

	/* The peer speaks first on an inbound connection, and names itself second. */
	peer.send("CSUP ADBASE ADTIGR");

	std::string line;
	EXO_ASSERT(peer.expect("CSUP ", line));
	EXO_ASSERT(peer.expect("CINF ", line));

	peer.send("CINF ID" PEER_CID);
	pump([&] { return session.isEstablished(); });

	EXO_ASSERT(session.isEstablished());

	/* The peer has gone, which on this host is a read error and not an end of file. */
	session.EventError(peer.theirs.get(),
		Samurai::IO::Net::SocketError::SocketRead, strerror(EPROTO));

	EXO_ASSERT(hub.errors == 0);

	QuickDC::Core::getInstance()->securityManager->revoke(
		QuickDC::Clearance::ADC, 0, PEER_CID, "job9");

	return 1;
});

/* The peer hanging up is noticed rather than spun on. */
EXO_TEST(clientsession_the_peer_hanging_up_is_survived,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);
	EXO_ASSERT(s.handshake());

	s.peer.mine.reset();
	pump([] { return false; }, 300);

	EXO_ASSERT(s.session != 0);
	return 1;
});

/*
 * A session still registered when the core goes down.
 *
 * This one is deliberately not deleted here: it is left for the connection
 * manager to tear down from ~Core, which is what happens when the window is
 * closed with a transfer running. A session owns its transfer and ~Transfer
 * unregisters from the transfer manager, so the manager has to outlive the
 * sessions - it did not, and every session's destructor then erased from a
 * vector that had already been destroyed.
 *
 * There is no assertion to make here beyond surviving: the crash was in
 * ~Core, which this file's teardown is what reaches. Run it under a sanitizer
 * and the bad ordering is a hard failure.
 */
EXO_TEST(clientsession_a_session_left_running_is_torn_down_with_the_core,
{
	QuickDC::QueueManager* queue = QuickDC::Core::getInstance()->queue;

	QuickDC::FilelistRequest* list =
		queue->requestFilelist("adc://127.0.0.1:1512", PEER_CID);
	EXO_ASSERT(list != 0);

	const char* token = queue->createRequest(list);
	EXO_ASSERT(token != 0);

	/*
	 * The peer's end has to outlive the case for the session to still be alive
	 * at shutdown, so this one is leaked on purpose too.
	 */
	Peer* peer = new Peer();
	if (!peer->connect()) EXO_SKIP(CANNOT_CONNECT);

	ADC::ClientSession* session =
		new ADC::ClientSession(peer->theirs, 0, token, 0, false, 0);

	/* Enough of a handshake to be given a transfer to hold. */
	peer->send("CSUP ADBASE ADTIGR\n");
	pump([] { return false; }, 60);

	std::string line;
	peer->expect("CINF ", line);

	peer->send("CINF ID" PEER_CID "\n");
	pump([] { return false; }, 60);

	/* We ask for the list; the peer says it is coming. */
	if (peer->expect("CGET ", line))
	{
		peer->send("CSND file files.xml.bz2 0 4096\n");
		pump([] { return false; }, 60);
	}

	(void) session;   /* left for ~Core to deal with */
	return 1;
});

/* Connections come and go without leaving anything in the monitor. */
EXO_TEST(clientsession_repeated_connections_leave_nothing_behind,
{
	Samurai::IO::Net::SocketMonitor* monitor =
		Samurai::IO::Net::SocketMonitor::getInstance();
	const size_t baseline = monitor->size();

	for (int n = 0; n < 3; n++)
	{
		Session s;
		if (!s.ready) EXO_SKIP(CANNOT_CONNECT);
		s.handshake();
	}

	pump([&] { return monitor->size() <= baseline; }, 2000);
	EXO_ASSERT(monitor->size() == baseline);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What a wrong token costs                                                  */
/* ------------------------------------------------------------------------- */

/*
 * A clearance is spent by the connection it authorised, and only by that one.
 *
 * Our CID goes out in every INF, so anyone on the hub can dial our peer port and
 * name us correctly - the token is the whole of the secret. What must not follow
 * from a wrong guess is the loss of the clearance a real transfer is waiting on,
 * because that would be a far cheaper way to stop a transfer than guessing
 * sixteen bytes is to steal one.
 */
EXO_TEST(clientsession_a_wrong_token_does_not_spend_the_clearance,
{
	Session s;
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	QuickDC::SecurityManager* security = QuickDC::Core::getInstance()->securityManager;
	EXO_ASSERT_NOT_NULL(security);

	/*
	 * Granted here rather than by the fixture: the fixture's token is also the
	 * session's own, and a session holding one uses it instead of whatever the
	 * peer quotes - so there would be no way for the peer to quote a wrong one.
	 */
	security->grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
		QuickDC::Clearance::ADC, 0, PEER_CID, "realjob"));

	EXO_ASSERT(s.handshake());
	s.say("CINF ID" PEER_CID " TOwrongjob");
	pump([] { return false; }, 200);

	/* Still there, and still the only thing that authorises the transfer. */
	const bool survived = security->isAuthorized(QuickDC::Clearance::ADC,
		0, 0, PEER_CID, "realjob");

	security->revoke(QuickDC::Clearance::ADC, 0, PEER_CID, "realjob");
	EXO_ASSERT(survived);
	return 1;
});

/* And the right one does spend it, so the case above is about the token being
   wrong rather than about clearances never being spent. */
EXO_TEST(clientsession_the_right_token_spends_the_clearance,
{
	Session s("realjob");
	if (!s.ready) EXO_SKIP(CANNOT_CONNECT);

	QuickDC::SecurityManager* security = QuickDC::Core::getInstance()->securityManager;
	EXO_ASSERT_NOT_NULL(security);

	EXO_ASSERT(s.handshake());
	s.say("CINF ID" PEER_CID);
	pump([] { return false; }, 200);

	EXO_ASSERT(!security->isAuthorized(QuickDC::Clearance::ADC,
		0, 0, PEER_CID, "realjob"));
	return 1;
});
