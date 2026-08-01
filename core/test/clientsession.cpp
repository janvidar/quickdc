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
 * The pair is made over the loopback interface, which has a consequence worth
 * knowing before reading the upload cases: SecurityManager::isAuthorized()
 * returns true for any loopback peer before it consults its table, so every
 * connection here is authorised whatever the token says. The clearance
 * matching itself is covered in security.cpp, where a null socket keeps that
 * shortcut out of the way.
 */

#include "quickdc.h"
#include "api/core.h"
#include "network/adc/client/adcclientsession.h"
#include "network/downloadqueue.h"
#include "network/securitymanager.h"

#include <samurai/io/file.h>
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

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const int TIMEOUT_MS = 5000;

/* 39 base32 characters, which is all CID::verify() asks for. */
#define PEER_CID "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define A_TTH    "AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"

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
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { }
		void EventCanWrite(const Samurai::IO::Net::Socket*) override { }
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override { }
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

			session = new ADC::ClientSession(peer.theirs, 0, token, 0, false, 0);
			ready = true;
		}

		~Session()
		{
			delete session;
		}

		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		void say(const std::string& command)
		{
			peer.send(command);
			pump([] { return false; }, 60);
		}

		/* The peer speaks first on an inbound connection. */
		bool handshake()
		{
			say("CSUP ADBASE ADTIGR");

			std::string line;
			if (!peer.expect("CSUP ", line)) return false;
			return peer.expect("CINF ", line);
		}
};

/* ------------------------------------------------------------------------- */

static void run()
{
	{
		Session s;
		CHECK(s.ready);
		printf("  ok a peer connection is accepted\n");
	}

	/* The peer opens with its support list, and we answer with ours. */
	{
		Session s;
		if (s.ready)
		{
			s.say("CSUP ADBASE ADTIGR");

			std::string line;
			CHECK(s.peer.expect("CSUP ", line));
			CHECK(line.find("ADBASE") != std::string::npos);
			printf("  ok CSUP is answered with CSUP: %s\n", line.c_str());
		}
		else failures++;
	}

	/* And with who we are. */
	{
		Session s;
		if (s.ready)
		{
			s.say("CSUP ADBASE ADTIGR");

			std::string line;
			CHECK(s.peer.expect("CINF ", line));
			CHECK(line.find("ID") != std::string::npos);
			CHECK(line.find("VE") != std::string::npos);
			printf("  ok and with CINF: %s\n", line.c_str());
		}
		else failures++;
	}

	/* A connection made for a job quotes that job's token back. */
	{
		Session s("thetoken");
		if (s.ready)
		{
			s.say("CSUP ADBASE");

			std::string line;
			CHECK(s.peer.expect("CINF ", line));
			CHECK(line.find("TOthetoken") != std::string::npos);
			printf("  ok a token is quoted back in our CINF\n");
		}
		else failures++;
	}

	/* Without one, there is nothing to quote. */
	{
		Session s;
		if (s.ready)
		{
			s.say("CSUP ADBASE");

			std::string line;
			CHECK(s.peer.expect("CINF ", line));
			CHECK(line.find(" TO") == std::string::npos);
			printf("  ok no token means none is quoted\n");
		}
		else failures++;
	}

	/*
	 * Asking for a file before saying who you are gets nothing. This is the
	 * one authorisation assertion the loopback shortcut does not decide:
	 * upload_approved is only ever set from CINF, which has not arrived.
	 */
	{
		Session s;
		if (s.ready)
		{
			s.say("CSUP ADBASE");

			std::string line;
			CHECK(s.peer.expect("CINF ", line));

			s.say("CGET file TTH/" A_TTH " 0 -1");
			pump([] { return false; }, 200);

			s.peer.drain();
			CHECK(s.peer.received.find("CSND") == std::string::npos);
			printf("  ok a file asked for before CINF is not sent\n");
		}
		else failures++;
	}

	/* A CINF whose CID is not a CID is not accepted as one. */
	{
		Session s;
		if (s.ready)
		{
			s.say("CSUP ADBASE");

			std::string line;
			CHECK(s.peer.expect("CINF ", line));

			s.say("CINF IDnotacid");
			s.say("CGET file TTH/" A_TTH " 0 -1");
			pump([] { return false; }, 200);

			s.peer.drain();
			CHECK(s.peer.received.find("CSND") == std::string::npos);
			printf("  ok a malformed CID does not identify a peer\n");
		}
		else failures++;
	}

	/*
	 * An upload is only ever considered once some token names a job, so these
	 * two carry one. What then approves it is the loopback shortcut rather
	 * than a granted clearance - see the file comment.
	 */
	{
		Session s("job1");
		if (s.ready && s.handshake())
		{
			s.say("CINF ID" PEER_CID);
			s.say("CGET file TTH/" A_TTH " 0 -1");

			std::string line;
			CHECK(s.peer.expect("CSTA ", line));
			printf("  ok a file we do not have is refused: %s\n", line.c_str());
		}
		else failures++;
	}

	/*
	 * Browsing someone asks for their whole share.
	 *
	 * A "list" request names a single directory, and a peer that implements it
	 * properly answers with that directory's subdirectories marked
	 * Incomplete="1" and nothing else - one folder and no way into it. The
	 * share entire is the rootless "files.xml.bz2" in the file namespace, so
	 * that is what a browse asks for.
	 */
	{
		QuickDC::QueueManager* queue = QuickDC::Core::getInstance()->queue;

		QuickDC::FilelistRequest* list =
			queue->requestFilelist("adc://127.0.0.1:1511", PEER_CID);
		CHECK(list != 0);

		/* The token is what ties the connection to the queued browse. */
		const char* token = (list) ? queue->createRequest(list) : 0;
		CHECK(token != 0);

		if (token)
		{
			Session s(token);
			if (s.ready && s.handshake())
			{
				s.say("CINF ID" PEER_CID);

				std::string line;
				CHECK(s.peer.expect("CGET ", line));
				CHECK(line == "CGET file files.xml.bz2 0 -1");
				printf("  ok a browse asks for the whole list: %s\n", line.c_str());
			}
			else failures++;
		}

		if (list) queue->removeFilelistRequest(list);
	}

	/* A well formed GET for a type nobody defined is refused, not acted on. */
	{
		Session s("job1");
		if (s.ready && s.handshake())
		{
			s.say("CINF ID" PEER_CID);
			s.say("CGET junk TTH/" A_TTH " 0 -1");

			std::string line;
			CHECK(s.peer.expect("CSTA ", line));
			printf("  ok an unknown GET type is refused\n");
		}
		else failures++;
	}

	/*
	 * A type that is not four characters is refused by the parser, before the
	 * session sees it - the type is read four characters deep whatever its
	 * length.
	 */
	{
		Session s("job1");
		if (s.ready && s.handshake())
		{
			s.say("CINF ID" PEER_CID);
			s.say("CGET ab TTH/" A_TTH " 0 -1");
			s.say("CGET toolongatype TTH/" A_TTH " 0 -1");
			pump([] { return false; }, 200);

			s.peer.drain();
			CHECK(s.peer.received.find("CSND") == std::string::npos);
			printf("  ok a type that is not four characters is refused\n");
		}
		else failures++;
	}

	/* Without a job named by either side there is nothing to approve. */
	{
		Session s;
		if (s.ready && s.handshake())
		{
			s.say("CINF ID" PEER_CID);
			s.say("CGET file TTH/" A_TTH " 0 -1");
			pump([] { return false; }, 300);

			s.peer.drain();
			CHECK(s.peer.received.find("CSND") == std::string::npos);
			CHECK(s.peer.received.find("CSTA") == std::string::npos);
			printf("  ok a connection for no job uploads nothing\n");
		}
		else failures++;
	}

	/* A status from the peer is read rather than answered in kind. */
	{
		Session s;
		if (s.ready && s.handshake())
		{
			s.say("CINF ID" PEER_CID);
			s.say("CSTA 151 File\\snot\\savailable");
			pump([] { return false; }, 200);
			printf("  ok a status from the peer is survived\n");
		}
		else failures++;
	}

	/* What a peer can throw at us. */
	{
		Session s;
		if (s.ready && s.handshake())
		{
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
			CHECK(s.session != 0);
			printf("  ok malformed and unknown commands are survived\n");
		}
		else failures++;
	}

	/* The peer hanging up is noticed rather than spun on. */
	{
		Session s;
		if (s.ready && s.handshake())
		{
			s.peer.mine.reset();
			pump([] { return false; }, 300);
			printf("  ok the peer hanging up is survived\n");
		}
		else failures++;
	}

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
	 * ~Core, long after this returns. Run it under a sanitizer and the bad
	 * ordering is a hard failure.
	 */
	{
		QuickDC::QueueManager* queue = QuickDC::Core::getInstance()->queue;

		QuickDC::FilelistRequest* list =
			queue->requestFilelist("adc://127.0.0.1:1512", PEER_CID);
		const char* token = (list) ? queue->createRequest(list) : 0;
		CHECK(token != 0);

		if (token)
		{
			/*
			 * The peer's end has to outlive the function for the session to still
			 * be alive at shutdown, so this one is leaked on purpose too.
			 */
			Peer* peer = new Peer();
			CHECK(peer->connect());

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
			printf("  ok a session left running is torn down with the core\n");
		}
	}

	/* Connections come and go without leaving anything in the monitor. */
	{
		Samurai::IO::Net::SocketMonitor* monitor =
			Samurai::IO::Net::SocketMonitor::getInstance();
		const size_t baseline = monitor->size();

		for (int n = 0; n < 3; n++)
		{
			Session s;
			CHECK(s.ready);
			if (s.ready) s.handshake();
		}

		pump([&] { return monitor->size() <= baseline; }, 2000);
		CHECK(monitor->size() == baseline);
		printf("  ok repeated connections leave nothing behind\n");
	}
}

int main()
{
	setbuf(stdout, 0);

	const std::string cfg = "/tmp/quickdc_peertest_config.conf";
	const std::string ses = "/tmp/quickdc_peertest_session.conf";
	const std::string queue = "/tmp/quickdc_peertest_queue.xml";

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();
	Samurai::IO::File(queue).remove();

	{
		QuickDC::Core core(cfg, ses, queue);
		run();
	}

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();
	Samurai::IO::File(queue).remove();
	Samurai::IO::File(queue + ".new").remove();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
