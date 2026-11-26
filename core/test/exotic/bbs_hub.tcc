/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The bulletin boards of the hub this program runs: what it announces, what it
 * describes, what it accepts and what it refuses.
 *
 * The hub here is the real one, listening on a loopback port the kernel picks and
 * driven by the real event loop, for the same reason the rest of the hub is tested
 * that way: it does not authenticate, so there is no state a case could set up
 * behind the protocol's back, and what the clients send is what an attacker sends.
 *
 * Boards come from the configuration, which the hub reads as it is constructed, so
 * a case writes its boards down before it builds the Server. Each case names its
 * own boards. A board's index is a file that outlives the hub - which is what one
 * of the cases below is about - so sharing a name between two cases would be one
 * case reading what the one before it left behind.
 *
 * The core is the reduced one, as in adchub.tcc: the full one scans shares and
 * opens listening sockets that none of this needs. It reads from disk, and so is a
 * per-file fixture; everything else a case needs it builds and lets go of again.
 */

#include "quickdc.h"

#include "api/bulletins.h"
#include "api/core.h"
#include "config/preferences.h"
#include "network/adc/adcbbs.h"
#include "network/adc/adccommon.h"
#include "network/adc/cid.h"
#include "network/adc/parser.h"
#include "network/adc/server/boards.h"
#include "network/adc/server/hub.h"
#include "network/adc/server/user.h"
#include "network/adc/server/claimcheck.h"

#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/dir.h>
#include <samurai/io/file.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/messagehandler.h>
#include <samurai/timer.h>
#include <samurai/util/base32.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <new>
#include <span>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <system_error>
#include <samurai/posix.h>
#include <vector>

/* The fixture's core; built in EXO_SETUP below. */
static QuickDC::Core* core = 0;

static const int TIMEOUT_MS = 5000;

/* ------------------------------------------------------------------------- */

/*
 * Runs the event loop until the condition holds, or gives up.
 *
 * The timers and the message queue of the core's loop are turned here as well
 * as its sockets, rather than through Core::run() and its quarter-second poll -
 * and the hub needs both.
 */
template<typename Fn>
static bool pump(Fn done, int timeout_ms = TIMEOUT_MS)
{
	Samurai::IO::Net::SocketMonitor* monitor = &core->loop().monitor();

	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);

	for (;;)
	{
		core->loop().timers().process();
		core->loop().messages().process();

		if (done()) return true;
		if (std::chrono::steady_clock::now() > deadline) return done();

		monitor->wait(10);
	}
}

/*
 * A CID and the PID it is the hash of, which is what the hub checks. Each client
 * needs its own: the hub refuses a second connection presenting a CID that is
 * already logged in.
 */
struct Identity
{
	std::string cid;
	std::string pid;
};

static Identity makeIdentity(uint8_t seed)
{
	uint8_t raw[Samurai::Crypto::Digest::TIGER_HASH_SIZE];
	memset(raw, seed, sizeof(raw));

	char pid[64] = {0,};
	char cid[64] = {0,};

	Samurai::Util::base32_encode(
		std::span<const unsigned char>(raw, sizeof(raw)),
		std::span<char>(pid, sizeof(pid)));

	Samurai::Crypto::Digest::Tiger tiger;
	tiger.update(raw, sizeof(raw));
	tiger.digest().getFormattedString(
		Samurai::Crypto::Digest::HashValue::Format::Base32, cid, sizeof(cid));

	Identity id;
	id.cid = cid;
	id.pid = pid;
	return id;
}

/**
 * A tiger tree root that is not any file's, but is one syntactically.
 *
 * The hub cannot check that a post document exists, that it hashes to what was
 * submitted or that it is the declared length - every one of those is required of
 * the client instead - so a hash that is well formed is all a case needs, and a
 * hash that is not is a case of its own.
 */
static std::string makeTTH(unsigned n)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

	std::string tth(39, 'A');
	tth[0] = alphabet[n % 32];
	tth[1] = alphabet[(n / 32) % 32];
	return tth;
}

/**
 * The value of a named field on a line the hub sent, or nothing.
 *
 * Read with the parser rather than looked for as a substring, because a value is
 * escaped on the way out - a subject with a space in it is not spelled on the wire
 * the way it was written - and because a field is only that field where it is one
 * whole argument.
 */
static std::string fieldOf(const std::string& line, const char* name)
{
	ADC::Command cmd;
	if (!ADC::Parser::parse(line.c_str(), line.size(), cmd)) return std::string();

	const char* value = cmd.getArgument(name, 2);
	return value ? std::string(value) : std::string();
}

/** How many lines the hub has said carry all of the given pieces. */
static size_t countLines(const std::vector<std::string>& log,
                         const std::vector<std::string>& pieces)
{
	size_t found = 0;

	for (size_t n = 0; n < log.size(); n++)
	{
		bool all = true;
		for (size_t p = 0; p < pieces.size() && all; p++)
			if (log[n].find(pieces[p]) == std::string::npos) all = false;

		if (all) found++;
	}

	return found;
}

/* Where a hub reads its access list and message of the day: never the user's own. */
static std::string hubHome()
{
	const std::string dir = "/tmp/quickdc_bbshubtest_" + std::to_string((int) getpid()) + "_hub";
	Samurai::IO::File::mkdir(dir.c_str(), 0700);
	return dir;
}

/* The hub, with what a case needs to reach made reachable. */
class TestHub : public ADC::LocalHub::Hub
{
	public:
		TestHub() : ADC::LocalHub::Hub(core->loop(), *core->config, hubHome()) { }

		using ADC::LocalHub::Hub::readConfiguration;
		using ADC::LocalHub::Hub::hasBoards;
};

/* The hub, and the listening socket that hands connections to it. */
class Server : public Samurai::IO::Net::ServerSocketEventHandler
{
	public:
		TestHub hub;
		std::shared_ptr<Samurai::IO::Net::ServerSocket> server;
		size_t accepted = 0;

		Server()
		{
			hub.enabled = true;
		}

		bool listen()
		{
			Samurai::IO::Net::InetSocketAddress any((uint16_t) 0);
			server = Samurai::IO::Net::ServerSocket::create(core->loop(), this, any);
			return server && server->getFD() != -1 && server->listen();
		}

		uint16_t port() const { return server ? server->getLocalPort() : 0; }

		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char*) override { }

		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*,
			std::shared_ptr<Samurai::IO::Net::Socket> socket) override
		{
			accepted++;
			hub.onConnected(socket);
			socket->setEventHandler(&hub);
		}
};

/* One connection to the hub, and whatever the hub has said on it. */
class Client : public Samurai::IO::Net::SocketEventHandler
{
	public:
		std::shared_ptr<Samurai::IO::Net::Socket> socket;
		std::string received;
		std::string sid;

		/** Every line the hub has said, login exchange included. */
		std::vector<std::string> log;

		bool connected = false;
		bool disconnected = false;

		~Client() override
		{
			if (socket) socket->setEventHandler(0);
		}

		bool connect(uint16_t port)
		{
			socket = Samurai::IO::Net::Socket::create(core->loop(), this, std::string("127.0.0.1"), port);
			if (!socket) return false;

			socket->connect();
			return pump([&] { return connected || disconnected; });
		}

		void send(const std::string& line)
		{
			if (!socket) return;
			const std::string out = line + "\n";
			socket->write(out.data(), out.size());
		}

		void drain()
		{
			if (!socket) return;
			for (;;)
			{
				char buf[4096];
				size_t got = 0;
				std::error_code ec;
				if (socket->read(buf, sizeof(buf), got, ec) != Samurai::IO::ReadResult::Ok) break;
				if (!got) break;
				received.append(buf, got);
			}
		}

		/** Waits for a whole line and takes it out of the stream. */
		bool nextLine(std::string& out, int timeout_ms = TIMEOUT_MS)
		{
			const bool got = pump([&] {
				drain();
				return received.find('\n') != std::string::npos;
			}, timeout_ms);

			if (!got) return false;

			const size_t at = received.find('\n');
			out = received.substr(0, at);
			received.erase(0, at + 1);
			log.push_back(out);
			return true;
		}

		/** True if any line the hub has said contains all of the given pieces. */
		bool sawLine(const std::vector<std::string>& pieces) const
		{
			return countLines(log, pieces) != 0;
		}

		/** Waits for a line whose first four bytes are the given fourcc. */
		bool waitFor(const char* fourcc, std::string& out, int timeout_ms = TIMEOUT_MS)
		{
			for (;;)
			{
				if (!nextLine(out, timeout_ms)) return false;
				if (out.compare(0, 4, fourcc) == 0) return true;
			}
		}

		/**
		 * Waits for a line carrying every one of the given pieces, reading past
		 * anything else - the descriptors and entries a busy board sends between
		 * one thing a case is waiting for and the next.
		 */
		bool waitForLine(const std::vector<std::string>& pieces, std::string& out,
			int timeout_ms = TIMEOUT_MS)
		{
			for (;;)
			{
				if (!nextLine(out, timeout_ms)) return false;

				bool all = true;
				for (size_t p = 0; p < pieces.size() && all; p++)
					if (out.find(pieces[p]) == std::string::npos) all = false;

				if (all) return true;
			}
		}

		/** Everything said so far, so that a case can count what follows. */
		void forget()
		{
			drain();
			received.clear();
			log.clear();
		}

		/**
		 * Read whatever has arrived into the log and stop.
		 *
		 * For a case that is about what the hub did *not* say, or about how many
		 * times it said something: waiting for one line leaves the rest in the
		 * stream, where sawLine() cannot see it.
		 */
		void settle(int timeout_ms = 400)
		{
			pump([] { return false; }, timeout_ms);

			std::string line;
			while (nextLine(line, 50)) { }
		}

		/**
		 * The whole login exchange, offering the bulletin board extension.
		 *
		 * BBS0 is negotiated in SUP and has no flag in INF: the field there says
		 * what other clients should know, and no client ever sends a bulletin to
		 * another. A session that does not offer it here is sent no descriptor,
		 * which is what 'bulletins' is for.
		 */
		bool login(const Identity& id, const char* nick, bool bulletins = true)
		{
			send(bulletins ? "HSUP ADBASE ADTIGR ADBBS0" : "HSUP ADBASE ADTIGR");

			std::string line;
			if (!waitFor("ISID", line)) return false;
			if (line.size() < 9) return false;
			sid = line.substr(5, 4);

			send("BINF " + sid + " ID" + id.cid + " PD" + id.pid + " NI" + nick);

			for (;;)
			{
				if (!nextLine(line)) return false;
				if (line.compare(0, 4, "BINF") == 0 && line.find(sid) != std::string::npos)
					return true;
				if (line.compare(0, 4, "ISTA") == 0 && line.find(" 2") != std::string::npos)
					return false;
			}
		}

		void EventHostLookup(const Samurai::IO::Net::Socket*) override { }
		void EventHostFound(const Samurai::IO::Net::Socket*) override { }
		void EventConnecting(const Samurai::IO::Net::Socket*) override { }
		void EventConnected(const Samurai::IO::Net::Socket*) override { connected = true; }
		void EventTimeout(const Samurai::IO::Net::Socket*) override { }
		void EventDisconnected(const Samurai::IO::Net::Socket*) override { disconnected = true; }
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override { drain(); }
		void EventCanWrite(const Samurai::IO::Net::Socket*) override { }
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override { }
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override { }
		void EventError(const Samurai::IO::Net::Socket*,
			Samurai::IO::Net::SocketError, const char*) override { disconnected = true; }
};

/* ------------------------------------------------------------------------- */

/* The fixture. Scratch paths of its own, so two of these at once do not collide. */
static std::string cfg;
static std::string ses;
static std::string bbs;

static std::string scratch(const char* leaf)
{
	return "/tmp/quickdc_bbshubtest_" + std::to_string((int) getpid()) + "_" + leaf;
}

/**
 * Write a board down, which is the whole of creating one.
 *
 * A section per board, named after it, read as the hub is constructed - so this
 * has to happen before the Server a case builds. The permissions are the PE of
 * the extension added up, stated per credential; a mask of 0 hides the board from
 * that credential entirely.
 */
static void configureBoard(const std::string& name, int userPermissions,
                           int maxSize = 262144, int maxEntries = 512,
                           int maxPerAuthor = 64)
{
	QuickDC::Preferences* config = core->config;
	const std::string group = "Local hub board " + name;

	config->setString(group, "Title", "Board " + name);
	config->setString(group, "Description", "A board for testing");
	config->setNumber(group, "Max post size", maxSize);
	config->setNumber(group, "Max entries", maxEntries);
	config->setNumber(group, "Max entries per author", maxPerAuthor);
	config->setNumber(group, "User permissions", userPermissions);
	config->setNumber(group, "Operator permissions", 31);
	config->setNumber(group, "Admin permissions", 31);
	config->setBool(group, "Registered only", false);
}

EXO_SETUP({
	cfg = scratch("config.conf");
	ses = scratch("session.conf");
	bbs = scratch("bbs");

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	std::error_code ignored;
	std::filesystem::remove_all(hubHome(), ignored);

	core = new QuickDC::Core(cfg, ses);
	if (core != core || !core->config) return 0;

	/* Not ~/.quickdc/hub/bbs: a test writes its indexes where it can throw them
	   away again, and where a developer's own boards are not what it reads. */
	core->config->setString("Local hub", "Bulletin directory", bbs);
	return 1;
});

EXO_TEARDOWN({
	/* Before the files: Preferences writes itself out when it is destroyed. */
	delete core;
	core = 0;

	Samurai::IO::File(cfg).remove();
	Samurai::IO::File(ses).remove();

	std::error_code ignored;
	std::filesystem::remove_all(hubHome(), ignored);

	/* Every index these cases wrote, and then the directory itself. */
	Samurai::IO::Directory dir(bbs);
	if (dir.open())
	{
		Samurai::IO::File entry;
		bool more = dir.first(entry);
		while (more)
		{
			const std::string path = entry.getName();
			more = dir.next(entry);
			Samurai::IO::File(path).remove();
		}
		dir.close();
	}

	Samurai::IO::File::rmdir(bbs.c_str());
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What the hub announces                                                    */
/* ------------------------------------------------------------------------- */

/*
 * The hub says it has boards, and says it in the SUP - which is the whole of the
 * negotiation, since BBS0 has no flag in INF. The announcement is authoritative:
 * a client must not send BBL or BBP to a hub that has not made it.
 */
EXO_TEST(bbs_hub_a_hub_with_a_board_announces_the_extension,
{
	configureBoard("announce", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(89), "reader"));

	/*
	 * On one line, with TIGR: the extension forbids announcing BBS0 without it,
	 * every identifier here being a tiger tree root, and a client without TIGR
	 * cannot fetch a post body.
	 */
	EXO_ASSERT(c.sawLine({"ISUP", "AD" ADC_HASH_TIGER, "AD" ADC_EXT_BULLETIN}));
	return 1;
});

/*
 * A hub built where something else lived is not left inside a broadcast. Reading the
 * configuration announces each board it finds, and that happened before the flag a
 * broadcast restores was given a value - so a hub made over used memory with a board
 * configured could believe for ever that it was walking its users.
 */
EXO_TEST(bbs_hub_a_hub_made_over_used_memory_is_not_left_inside_a_broadcast,
{
	configureBoard("dirty", 15);

	void* raw = ::operator new(sizeof(TestHub));
	memset(raw, 0xff, sizeof(TestHub));
	TestHub* hub = new (raw) TestHub();

	EXO_ASSERT(hub->hasBoards());
	EXO_ASSERT(!hub->processing);

	hub->~TestHub();
	::operator delete(raw);
	return 1;
});

/*
 * And a hub whose index directory cannot be made does not announce it.
 *
 * A hub must not announce what it cannot do: the announcement is what permits a
 * client to send BBL and BBP at all, and everything it then sent could only be
 * refused. The directory here has a file for a parent, so making it fails for a
 * reason no configuration can talk its way out of.
 */
EXO_TEST(bbs_hub_a_hub_whose_store_is_unusable_announces_nothing,
{
	configureBoard("unusable", 15);

	QuickDC::Preferences* config = core->config;
	config->setString("Local hub", "Bulletin directory", "/dev/null/boards");

	{
		Server s;
		if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

		EXO_ASSERT(!s.hub.hasBoards());

		Client c;
		EXO_ASSERT(c.connect(s.port()));
		EXO_ASSERT(c.login(makeIdentity(90), "nobody"));

		EXO_ASSERT(c.sawLine({"ISUP"}));
		EXO_ASSERT(!c.sawLine({"ISUP", "AD" ADC_EXT_BULLETIN}));
		EXO_ASSERT(!c.sawLine({"IBBD"}));

		/* And the commands are refused rather than acted on, with the feature error
		   that names the command it applies to. */
		c.send("HBBL BDunusable TS0");

		std::string sta;
		EXO_ASSERT(c.waitFor("ISTA", sta));
		EXO_ASSERT(sta.find("145") != std::string::npos);
		EXO_ASSERT(sta.find("FCBBL") != std::string::npos);
	}

	config->setString("Local hub", "Bulletin directory", bbs);
	return 1;
});

/*
 * A hub with no board configured announces nothing either. Writing the first
 * board down is how an operator turns the feature on, and a hub with none has
 * nothing to subscribe to.
 *
 * The store is replaced rather than the configuration emptied: a section with the
 * keys taken out of it is still a section, so a board cannot be un-written from
 * under a hub that has already read it.
 */
EXO_TEST(bbs_hub_a_hub_with_no_boards_announces_nothing,
{
	configureBoard("unannounced", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	EXO_ASSERT(s.hub.hasBoards());

	delete s.hub.boards;
	s.hub.boards = new ADC::LocalHub::BoardStore(bbs);

	EXO_ASSERT(s.hub.boards->isUsable());
	EXO_ASSERT_EQ_UINT(s.hub.boards->count(), 0u);
	EXO_ASSERT(!s.hub.hasBoards());

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(91), "nobody"));

	EXO_ASSERT(c.sawLine({"ISUP"}));
	EXO_ASSERT(!c.sawLine({"ISUP", "AD" ADC_EXT_BULLETIN}));

	/* And the session is told about nothing. */
	EXO_ASSERT(!c.sawLine({"IBBD"}));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What the hub describes                                                    */
/* ------------------------------------------------------------------------- */

/*
 * A descriptor comes after the session's own INF has been sent back to it, and
 * this is the case that says so.
 *
 * That echo is what tells a client its login finished, and a client is entitled
 * to refuse a descriptor arriving before it - a hub naming a board to a session
 * it has not accepted yet is the thing being refused. Sent a moment early, every
 * descriptor is dropped and the whole extension goes quiet on a client that is
 * behaving correctly, which is a failure no amount of testing either side on its
 * own would find.
 */
EXO_TEST(bbs_hub_a_descriptor_comes_after_the_session_has_had_its_own_inf,
{
	configureBoard("ordered", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));

	/* Stops at its own INF, so anything read past here followed it. */
	EXO_ASSERT(c.login(makeIdentity(2), "reader"));

	std::string bbd;
	EXO_ASSERT(c.waitForLine({"IBBD", "BDordered"}, bbd));
	return 1;
});

/*
 * One descriptor per board, after the INF exchange, to a session that asked for
 * them - the pattern by which a hub sends the INF of every user at login.
 */
EXO_TEST(bbs_hub_a_session_that_offered_the_extension_is_told_what_there_is,
{
	configureBoard("described", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(1), "reader"));

	/*
	 * Waited for rather than looked for among what login already read: a
	 * descriptor goes out after the session's own INF has been sent back to it,
	 * which is the point login() returns at.
	 */
	std::string bbd;

	/* One line, carrying the lot: what the session may do, the largest post the
	   board will take, where the board is now, how far back it will replay, and
	   how much it holds. */
	EXO_ASSERT(c.waitForLine({"IBBD", "BDdescribed", "PE15", "MS262144",
		"TS0", "OT0", "NP0"}, bbd));
	return 1;
});

/* A session that did not offer the extension is told nothing at all. */
EXO_TEST(bbs_hub_a_session_that_did_not_offer_the_extension_is_told_nothing,
{
	configureBoard("quiet", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(2), "reader", false));

	/*
	 * Read to the end before deciding nothing arrived. login() returns on the session's
	 * own INF echo and sawLine() searches only what has been read, so without this the
	 * descriptors would still be on the socket and their absence from the log would say
	 * nothing about whether the hub sent them.
	 */
	c.settle();

	EXO_ASSERT(!c.sawLine({"IBBD"}));
	return 1;
});

/*
 * And a session that offers the extension after it has logged in is told then.
 *
 * The descriptors go out on entry to the NORMAL state, and a session that was not
 * asking for them at that point would otherwise never learn there was anything to
 * read. Only the boards it has not already been told about, so that an offer
 * repeated is not a burst of one line per board repeated.
 */
EXO_TEST(bbs_hub_a_session_that_offers_the_extension_late_is_told_then,
{
	configureBoard("late", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(92), "latecomer", false));
	EXO_ASSERT(!c.sawLine({"IBBD"}));

	c.forget();
	c.send("HSUP AD" ADC_EXT_BULLETIN);
	c.settle();

	EXO_ASSERT_EQ_UINT(countLines(c.log, {"IBBD", "BDlate"}), 1u);

	/* Offered again, and again, and said once. */
	c.forget();
	c.send("HSUP AD" ADC_EXT_BULLETIN);
	c.send("HSUP AD" ADC_EXT_BULLETIN);
	c.settle();

	EXO_ASSERT_EQ_UINT(countLines(c.log, {"IBBD", "BDlate"}), 0u);

	/* And taking it back ends every subscription on the connection, which is what
	   the extension has a hub do when it withdraws the feature itself. */
	c.send("HBBL BDlate TS0");
	c.settle();
	EXO_ASSERT(!c.sawLine({"ISTA", "FCBBL"}));

	c.send("HSUP RM" ADC_EXT_BULLETIN);
	c.settle();

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(93), "alice"));

	c.forget();
	const std::string tth = makeTTH(37);
	alice.send("HBBP TR" + tth + " SI100 BDlate SJNot\\sfor\\syou");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));

	c.settle();
	EXO_ASSERT(!c.sawLine({"IBBL"}));
	return 1;
});

/*
 * A board a session may not subscribe to gets no descriptor at all.
 *
 * Withholding the descriptor is the whole of the mechanism for hiding a board, so
 * there is nothing else to get right - and the board answers a subscription as
 * though it were not there, since a refusal that told a hidden board from an
 * absent one would undo the hiding.
 */
EXO_TEST(bbs_hub_a_board_without_permission_to_subscribe_is_not_described,
{
	/* 2 and 4: post and reply, and no permission 1. */
	configureBoard("hidden", 6);
	configureBoard("visible", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(3), "reader"));

	/*
	 * The visible one is waited for first, and the hidden one is only then said
	 * to be absent. Both go out in one pass, so having seen the second is what
	 * makes the absence of the first mean anything - asked before the pass, every
	 * board is equally undescribed and the case would pass for the wrong reason.
	 */
	std::string bbd;
	EXO_ASSERT(c.waitForLine({"IBBD", "BDvisible"}, bbd));
	EXO_ASSERT(!c.sawLine({"IBBD", "BDhidden"}));

	/* And it is refused, with the code for a board that is not there. */
	c.send("HBBL BDhidden TS0");

	std::string sta;
	EXO_ASSERT(c.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("125") != std::string::npos);
	EXO_ASSERT(sta.find("FCBBL") != std::string::npos);
	EXO_ASSERT(sta.find("BDhidden") != std::string::npos);
	return 1;
});

/* A board nobody may do anything on is not there as far as a session can tell. */
EXO_TEST(bbs_hub_a_board_with_no_permissions_at_all_answers_as_absent,
{
	configureBoard("sealed", 0);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client c;
	EXO_ASSERT(c.connect(s.port()));
	EXO_ASSERT(c.login(makeIdentity(4), "reader"));

	EXO_ASSERT(!c.sawLine({"IBBD", "BDsealed"}));

	c.send("HBBL BDsealed TS0");

	std::string sta;
	EXO_ASSERT(c.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("171") != std::string::npos);
	EXO_ASSERT(sta.find("BDsealed") != std::string::npos);

	/* Which is what a board that really is absent answers, and by design the two
	   cannot be told apart. */
	c.send("HBBL BDnosuchboard TS0");
	EXO_ASSERT(c.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("171") != std::string::npos);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Posting                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * A post reaches every subscriber, and the entry is what says the hub took it.
 *
 * There is no success status message in this extension. The entry is the
 * acknowledgement, so the author sees exactly what every other subscriber sees.
 */
EXO_TEST(bbs_hub_a_post_reaches_every_subscriber,
{
	configureBoard("posted", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	const Identity alice_id = makeIdentity(10);

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(alice_id, "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(11), "bob"));

	alice.send("HBBL BDposted TS0");
	bob.send("HBBL BDposted TS0");
	pump([] { return false; }, 200);

	const std::string tth = makeTTH(1);
	alice.send("HBBP TR" + tth + " SI412 BDposted SJHub\\supgrade");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));

	/* What the author declared, and what the hub attests to. */
	EXO_ASSERT(fieldOf(line, "TR") == tth);
	EXO_ASSERT(fieldOf(line, "SI") == "412");
	EXO_ASSERT(fieldOf(line, "BD") == "posted");
	EXO_ASSERT(fieldOf(line, "ID") == alice_id.cid);
	EXO_ASSERT(fieldOf(line, "NI") == "alice");
	EXO_ASSERT(fieldOf(line, "SJ") == "Hub upgrade");
	EXO_ASSERT(!fieldOf(line, "TS").empty());

	/* No parent, so the post starts a thread whose root is its own hash - which
	   the hub worked out from the absence of PA. */
	EXO_ASSERT(fieldOf(line, "TH") == tth);
	EXO_ASSERT(fieldOf(line, "PA").empty());

	std::string other;
	EXO_ASSERT(bob.waitForLine({"IBBL", "TR" + tth}, other));
	EXO_ASSERT(fieldOf(other, "ID") == alice_id.cid);
	return 1;
});

/*
 * And it reaches the session that submitted it whether that session subscribes or
 * not, because the entry is the only acknowledgement there is: a client that
 * posted and was told nothing cannot tell acceptance from a refusal that went
 * missing.
 */
EXO_TEST(bbs_hub_the_submitting_session_receives_the_entry_without_subscribing,
{
	configureBoard("unsubscribed", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(12), "alice"));

	const std::string tth = makeTTH(2);
	alice.send("HBBP TR" + tth + " SI100 BDunsubscribed SJNothing\\sfollowed");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));
	EXO_ASSERT(fieldOf(line, "BD") == "unsubscribed");
	return 1;
});

/*
 * The thread of a reply is the parent's, and it is the hub's to work out.
 *
 * TH is not submitted because it is not the client's to compute, and a client that
 * sends one anyway is not refused for it: the field is discarded, in the way the
 * hub discards PD from an INF.
 */
EXO_TEST(bbs_hub_a_reply_takes_its_thread_from_its_parent,
{
	configureBoard("threaded", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(13), "alice"));

	const std::string root = makeTTH(3);
	const std::string reply = makeTTH(4);
	const std::string lie = makeTTH(5);

	alice.send("HBBP TR" + root + " SI100 BDthreaded SJRoot");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + root}, line));
	EXO_ASSERT(fieldOf(line, "TH") == root);

	alice.send("HBBP TR" + reply + " SI120 BDthreaded PA" + root
		+ " TH" + lie + " SJRe:\\sRoot");

	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + reply}, line));
	EXO_ASSERT(fieldOf(line, "PA") == root);
	EXO_ASSERT(fieldOf(line, "TH") == root);
	EXO_ASSERT(fieldOf(line, "TH") != lie);

	/*
	 * And a reply whose parent this board has never seen starts a thread of its
	 * own: the parent may be on another hub, and there is no thread here to attach
	 * it to. Accepted rather than refused, which is what makes a board's posts the
	 * same posts wherever they are indexed.
	 */
	const std::string orphan = makeTTH(6);
	alice.send("HBBP TR" + orphan + " SI130 BDthreaded PA" + makeTTH(30) + " SJOrphan");

	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + orphan}, line));
	EXO_ASSERT(fieldOf(line, "TH") == orphan);
	return 1;
});

/*
 * ID, NI, TH and TS in a submission are discarded and not refused.
 *
 * The extension is explicit about this, and it is the same treatment PD gets in an
 * INF: a client that says something that is not its to say has its claim dropped
 * rather than its command rejected. The hub builds the entry from nothing, which
 * is what makes discarding them the default instead of something to remember.
 */
EXO_TEST(bbs_hub_the_fields_a_client_may_not_send_are_discarded,
{
	configureBoard("discarded", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	const Identity alice_id = makeIdentity(14);
	const Identity other_id = makeIdentity(15);

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(alice_id, "alice"));

	const std::string tth = makeTTH(7);

	alice.send("HBBP TR" + tth + " SI140 BDdiscarded SJClaims"
		+ " ID" + other_id.cid + " NImallory TH" + makeTTH(31) + " TS1");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));

	EXO_ASSERT(fieldOf(line, "ID") == alice_id.cid);
	EXO_ASSERT(fieldOf(line, "NI") == "alice");
	EXO_ASSERT(fieldOf(line, "TH") == tth);
	EXO_ASSERT(fieldOf(line, "TS") != "1");

	/* Nothing was refused: the command was carried out, with four of its fields
	   ignored. */
	EXO_ASSERT(!alice.sawLine({"ISTA", "FCBBP"}));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What a submission is refused for                                          */
/* ------------------------------------------------------------------------- */

/* A board where only an operator may start a thread, which is the natural shape
   of an announcements board - and the reason 2 is separate from 4. */
EXO_TEST(bbs_hub_a_post_without_the_permission_for_it_is_refused,
{
	/* 1 and 4: read and reply, and no permission to start a thread. */
	configureBoard("announcements", 5);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(16), "alice"));

	const std::string tth = makeTTH(8);
	alice.send("HBBP TR" + tth + " SI100 BDannouncements SJA\\sthread");

	std::string sta;
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("125") != std::string::npos);
	EXO_ASSERT(sta.find("FCBBP") != std::string::npos);
	EXO_ASSERT(sta.find("TR" + tth) != std::string::npos);

	/*
	 * 125 is severity 1 and ADC_CODE_ACCESS_DENIED, which a board that does not exist
	 * or that nobody may use answers with 171 and 170 instead - so the refusal above is
	 * about this permission on this board and not about the board.
	 *
	 * And PA does not get round it. A post naming a parent this board does not hold is a
	 * thread as far as this board is concerned - that is what BoardStore::add() makes of
	 * it - so it needs the permission to start one. Decided from PA being present rather
	 * than from PA resolving, a session with Reply and not StartThread may post at the top
	 * of an announcements board by quoting any hash at all, and be charged at the reply
	 * rate for doing it.
	 */
	const std::string orphan = makeTTH(9);
	alice.send("HBBP TR" + orphan + " SI100 BDannouncements PA" + tth
		+ " SJNot\\sa\\sreply");

	std::string refused;
	EXO_ASSERT(alice.waitFor("ISTA", refused));
	EXO_ASSERT(refused.find("125") != std::string::npos);
	EXO_ASSERT(refused.find("FCBBP") != std::string::npos);
	EXO_ASSERT(refused.find("TR" + orphan) != std::string::npos);

	/* And it is not on the board, which is the half a status code cannot show. */
	std::string line;
	EXO_ASSERT(!alice.sawLine({"IBBL", "TR" + orphan}));
	return 1;
});

/* A post past the board's limit, refused against the limit so a client can say
   what went wrong without reading English. */
EXO_TEST(bbs_hub_a_post_past_the_board_limit_is_refused_against_it,
{
	configureBoard("small", 15, 4096);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(17), "alice"));

	const std::string tth = makeTTH(10);
	alice.send("HBBP TR" + tth + " SI99999 BDsmall SJToo\\sbig");

	std::string sta;
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("172") != std::string::npos);
	EXO_ASSERT(sta.find("MS4096") != std::string::npos);
	EXO_ASSERT(sta.find("TR" + tth) != std::string::npos);
	return 1;
});

/*
 * What a board takes where its configuration says nothing, which is two megabytes.
 *
 * The number is not a guess at how long a post is - it is enormous for a header and
 * some text. It is the bound that makes serving a post without an upload slot
 * affordable, so it and Share::Manager::SlotCost::Free are chosen together, and
 * moving this without looking at that is the mistake to avoid. Configured here by
 * writing the group without the key rather than through configureBoard(), which
 * always writes one and would test its own default instead of the hub's.
 */
EXO_TEST(bbs_hub_a_board_that_says_nothing_takes_two_megabytes,
{
	QuickDC::Preferences* config = core->config;
	config->setString("Local hub board unbounded", "Title", "Unbounded");
	config->setNumber("Local hub board unbounded", "User permissions", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(41), "alice"));

	std::string bbd;
	EXO_ASSERT(alice.waitForLine({"IBBD", "BDunbounded"}, bbd));
	EXO_ASSERT(fieldOf(bbd, "MS") == "2097152");

	/* And it is the limit that is enforced, not merely the one announced. */
	const std::string tth = makeTTH(41);
	alice.send("HBBP TR" + tth + " SI2097153 BDunbounded SJOne\\sbyte\\stoo\\smany");

	std::string sta;
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("172") != std::string::npos);
	EXO_ASSERT(sta.find("MS2097152") != std::string::npos);

	/* A post of exactly the limit is not past it. */
	const std::string fits = makeTTH(42);
	alice.send("HBBP TR" + fits + " SI2097152 BDunbounded SJExactly\\sthe\\slimit");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + fits}, line));
	return 1;
});

/* The same hash twice on one board is one post. */
EXO_TEST(bbs_hub_a_post_already_indexed_is_refused,
{
	configureBoard("duplicated", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(18), "alice"));

	const std::string tth = makeTTH(11);
	alice.send("HBBP TR" + tth + " SI100 BDduplicated SJOnce");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));

	alice.send("HBBP TR" + tth + " SI100 BDduplicated SJTwice");

	std::string sta;
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("170") != std::string::npos);
	EXO_ASSERT(sta.find("TR" + tth) != std::string::npos);
	return 1;
});

/*
 * A hash that is not one, and the fields that are missing.
 *
 * Base32 at this length has no padding and no lower case, so a lower case root and
 * one containing 0, 1, 8 or 9 are refused rather than folded: two spellings of one
 * hash would be two names for one post.
 */
EXO_TEST(bbs_hub_a_malformed_submission_is_refused_by_the_field_that_is_wrong,
{
	configureBoard("malformed", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(19), "alice"));

	std::string sta;

	alice.send("HBBP TRnotahash SI100 BDmalformed SJBad");
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("143") != std::string::npos);
	EXO_ASSERT(sta.find("FBTR") != std::string::npos);

	/* Lower case, which is the same hash to a human and not to base32. */
	std::string lower = makeTTH(12);
	lower[0] = 'a';
	alice.send("HBBP TR" + lower + " SI100 BDmalformed SJBad");
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("143") != std::string::npos);
	EXO_ASSERT(sta.find("FBTR") != std::string::npos);

	/* No hash at all, and no size, each named as the field that is missing. */
	alice.send("HBBP SI100 BDmalformed SJBad");
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("143") != std::string::npos);
	EXO_ASSERT(sta.find("FMTR") != std::string::npos);

	alice.send("HBBP TR" + makeTTH(13) + " BDmalformed SJBad");
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("143") != std::string::npos);
	EXO_ASSERT(sta.find("FMSI") != std::string::npos);

	/* A parent that is not a hash, and a board name that is not one. */
	alice.send("HBBP TR" + makeTTH(14) + " SI100 BDmalformed PAnope SJBad");
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("143") != std::string::npos);
	EXO_ASSERT(sta.find("FBPA") != std::string::npos);

	alice.send("HBBP TR" + makeTTH(15) + " SI100 BD../../etc SJBad");
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("143") != std::string::npos);
	EXO_ASSERT(sta.find("FBBD") != std::string::npos);

	/* And the hub is still the hub it was. */
	const std::string tth = makeTTH(16);
	alice.send("HBBP TR" + tth + " SI100 BDmalformed SJGood");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));
	return 1;
});

/*
 * Posting is rate limited, and starting a thread more strictly than replying.
 *
 * The hub is the only place where posting can be refused. An existing conversation
 * is bounded by the interest of the people in it and thread creation is not, which
 * is why the two are counted apart - and why a hub under load falls back to a
 * board where anyone may reply and only operators may post.
 */
EXO_TEST(bbs_hub_posting_is_rate_limited_and_a_thread_more_strictly,
{
	configureBoard("flooded", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(20), "alice"));

	std::string line;
	std::string root;

	/* What the thread allowance holds at once. */
	for (unsigned n = 0; n < (unsigned) ADC::LocalHub::BOARD_THREAD_BURST; n++)
	{
		const std::string tth = makeTTH(40 + n);
		if (root.empty()) root = tth;

		alice.send("HBBP TR" + tth + " SI100 BDflooded SJThread");
		if (!alice.waitForLine({"IBBL", "TR" + tth}, line))
			EXO_FAIL("a thread within the allowance was not accepted");
	}

	/* And one more, refused with the seconds to wait rather than a disconnection. */
	alice.send("HBBP TR" + makeTTH(60) + " SI100 BDflooded SJOne\\stoo\\smany");

	std::string sta;
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("175") != std::string::npos);
	EXO_ASSERT(sta.find(" TL") != std::string::npos);

	/*
	 * A reply spends the post allowance and not the thread one, so replying still
	 * works where starting a thread has stopped - which is the distinction the whole
	 * arrangement exists for. What is left of it is what the threads did not spend.
	 */
	const unsigned left = (unsigned) ADC::LocalHub::BOARD_POST_BURST
		- (unsigned) ADC::LocalHub::BOARD_THREAD_BURST;

	for (unsigned n = 0; n < left; n++)
	{
		const std::string reply = makeTTH(61 + n);
		alice.send("HBBP TR" + reply + " SI100 BDflooded PA" + root + " SJA\\sreply");

		if (!alice.waitForLine({"IBBL", "TR" + reply}, line))
			EXO_FAIL("a reply within the allowance was not accepted");
	}

	/* And then replying stops too. A few more, so that the refusal does not depend
	   on the second the test happens to run in. */
	alice.forget();
	for (unsigned n = 0; n < 4; n++)
		alice.send("HBBP TR" + makeTTH(70 + n) + " SI100 BDflooded PA" + root
			+ " SJAnother");

	alice.settle();
	EXO_ASSERT(alice.sawLine({"ISTA", "175", "FCBBP"}));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Withdrawal                                                                */
/* ------------------------------------------------------------------------- */

/*
 * A withdrawal is a tombstone with a later timestamp, and that is what makes it
 * work: it places the record at the head of the stream, so it reaches a subscriber
 * at once and an absent client as soon as it resumes. The original timestamp would
 * have put it behind every cursor that had already moved past the entry.
 *
 * Later even when the withdrawal happens in the same second as the post, because
 * a client replaces an entry it holds only when the timestamp has moved on.
 */
EXO_TEST(bbs_hub_a_withdrawal_reaches_a_subscriber_as_a_tombstone,
{
	configureBoard("withdrawn", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(21), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(22), "bob"));

	bob.send("HBBL BDwithdrawn TS0");
	pump([] { return false; }, 200);

	const std::string tth = makeTTH(17);
	alice.send("HBBP TR" + tth + " SI100 BDwithdrawn SJRegretted");

	std::string entry;
	EXO_ASSERT(bob.waitForLine({"IBBL", "TR" + tth}, entry));
	const std::string was = fieldOf(entry, "TS");
	EXO_ASSERT(!was.empty());

	alice.send("HBBP TR" + tth + " BDwithdrawn RM1");

	std::string stone;
	EXO_ASSERT(bob.waitForLine({"IBBL", "TR" + tth, "RM1"}, stone));

	/* A tombstone carries the hash, the board, the timestamp and the flag, and
	   should carry nothing else: the subject and the author of a withdrawn post
	   are usually the reason it was withdrawn. */
	EXO_ASSERT(fieldOf(stone, "BD") == "withdrawn");
	EXO_ASSERT(fieldOf(stone, "SJ").empty());
	EXO_ASSERT(fieldOf(stone, "ID").empty());
	EXO_ASSERT(fieldOf(stone, "NI").empty());

	const uint64_t before = strtoull(was.c_str(), 0, 10);
	const uint64_t after = strtoull(fieldOf(stone, "TS").c_str(), 0, 10);
	EXO_ASSERT(after > before);

	/*
	 * And a client that asks after it is answered with the tombstone rather than
	 * told there is no such entry. The tombstone is an index entry - it is what the
	 * board holds for that hash now - and a client that has just been told the
	 * parent of an orphaned reply was withdrawn knows to stop asking for it, where
	 * one told the hash is unknown does not.
	 */
	bob.send("HBBL BDwithdrawn TR" + tth);

	std::string asked;
	EXO_ASSERT(bob.waitForLine({"IBBL", "TR" + tth, "RM1"}, asked));
	EXO_ASSERT(fieldOf(asked, "SJ").empty());
	return 1;
});

/* Somebody else's post needs permission 16, and one's own needs 8. */
EXO_TEST(bbs_hub_withdrawing_somebody_elses_post_needs_the_permission_for_it,
{
	/* 1, 2, 4 and 8: read, post, reply and withdraw one's own. */
	configureBoard("moderated", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(23), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(24), "bob"));

	const std::string tth = makeTTH(18);
	alice.send("HBBP TR" + tth + " SI100 BDmoderated SJAlice\\sspeaks");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));

	/* Bob holds 8 and not 16, and this post is not his. */
	bob.send("HBBP TR" + tth + " BDmoderated RM1");

	std::string sta;
	EXO_ASSERT(bob.waitForLine({"ISTA"}, sta));
	EXO_ASSERT(sta.find("125") != std::string::npos);
	EXO_ASSERT(sta.find("TR" + tth) != std::string::npos);

	/* Alice's own, which 8 is exactly the permission for. */
	alice.send("HBBP TR" + tth + " BDmoderated RM1");
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth, "RM1"}, line));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Subscribing                                                               */
/* ------------------------------------------------------------------------- */

/*
 * A client resumes from the highest timestamp it holds and not from one second
 * later, so the entries of that second arrive again. Timestamps are not unique,
 * and a cursor advanced past a second would step over everything else accepted in
 * it - which is why the duplicate is the cheap half of the trade.
 */
EXO_TEST(bbs_hub_a_resumption_replays_the_second_it_resumes_from,
{
	configureBoard("resumed", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(25), "alice"));

	const std::string tth = makeTTH(19);
	alice.send("HBBP TR" + tth + " SI100 BDresumed SJOne");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));

	const std::string at = fieldOf(line, "TS");
	EXO_ASSERT(!at.empty());

	alice.forget();
	alice.send("HBBL BDresumed TS" + at);

	std::string again;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, again));
	EXO_ASSERT(fieldOf(again, "TS") == at);

	/* And one second later is where it would have been lost. */
	alice.forget();
	alice.send("HBBL BDresumed TS" + std::to_string(strtoull(at.c_str(), 0, 10) + 1));
	pump([] { return false; }, 300);
	alice.drain();
	EXO_ASSERT(alice.received.find("TR" + tth) == std::string::npos);
	return 1;
});

/*
 * A second BBL naming a board already followed replaces the first rather than
 * adding to it, and replays from the newly requested timestamp. Added to, a client
 * that resubscribed would be sent every entry twice for the rest of the session.
 */
EXO_TEST(bbs_hub_a_second_request_replaces_the_subscription_it_repeats,
{
	configureBoard("replaced", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	Client bob;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(26), "alice"));
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(27), "bob"));

	bob.send("HBBL BDreplaced TS0");
	bob.send("HBBL BDreplaced TS0");
	bob.send("HBBL BDreplaced TS0");
	bob.settle();

	/* Three requests, no refusal: a repeat replaces and so is never one too many. */
	EXO_ASSERT(!bob.sawLine({"ISTA", "FCBBL"}));

	bob.forget();

	const std::string tth = makeTTH(20);
	alice.send("HBBP TR" + tth + " SI100 BDreplaced SJOnce\\sonly");

	std::string line;
	EXO_ASSERT(bob.waitForLine({"IBBL", "TR" + tth}, line));

	/* Long enough for a second copy to have arrived, if there were two
	   subscriptions to send it down. */
	bob.settle();
	EXO_ASSERT_EQ_UINT(countLines(bob.log, {"IBBL", "TR" + tth}), 1u);
	return 1;
});

/* A request carrying both TR and TS is a question and a subscription at once,
   which is neither. */
EXO_TEST(bbs_hub_a_request_naming_a_post_and_a_time_is_malformed,
{
	configureBoard("both", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(28), "alice"));

	alice.send("HBBL BDboth TR" + makeTTH(21) + " TS0");

	std::string sta;
	EXO_ASSERT(alice.waitFor("ISTA", sta));
	EXO_ASSERT(sta.find("140") != std::string::npos);
	EXO_ASSERT(sta.find("FCBBL") != std::string::npos);
	EXO_ASSERT(sta.find("BDboth") != std::string::npos);
	return 1;
});

/*
 * A question about one entry is answered with that entry, or with the code for an
 * entry the hub does not hold - and it changes no subscription either way, which is
 * how a client fills in the parent of a reply that arrived orphaned.
 */
EXO_TEST(bbs_hub_a_question_about_one_entry_is_answered_and_changes_nothing,
{
	configureBoard("asked", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(29), "alice"));

	const std::string tth = makeTTH(22);
	alice.send("HBBP TR" + tth + " SI100 BDasked SJAsked\\sabout");

	std::string line;
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));

	alice.forget();
	alice.send("HBBL BDasked TR" + tth);
	EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));
	EXO_ASSERT(fieldOf(line, "SJ") == "Asked about");

	/* One the hub does not hold. */
	alice.send("HBBL BDasked TR" + makeTTH(23));

	std::string sta;
	EXO_ASSERT(alice.waitForLine({"ISTA"}, sta));
	EXO_ASSERT(sta.find("176") != std::string::npos);
	EXO_ASSERT(sta.find("TR" + makeTTH(23)) != std::string::npos);

	/*
	 * And no subscription was taken up by either: a second post arrives at a
	 * session that only ever asked questions if the question subscribed it.
	 */
	Client bob;
	EXO_ASSERT(bob.connect(s.port()));
	EXO_ASSERT(bob.login(makeIdentity(38), "bob"));

	alice.forget();
	const std::string second = makeTTH(24);
	bob.send("HBBP TR" + second + " SI100 BDasked SJUnasked\\sfor");
	EXO_ASSERT(bob.waitForLine({"IBBL", "TR" + second}, line));

	pump([] { return false; }, 300);
	alice.drain();
	EXO_ASSERT(alice.received.find("TR" + second) == std::string::npos);
	return 1;
});

/*
 * How many boards one session may follow at once. A subscription is a standing
 * instruction to send data, obtained with one short command and available on every
 * board at the same time.
 */
EXO_TEST(bbs_hub_a_session_follows_at_most_the_subscription_ceiling,
{
	const size_t ceiling = ADC::LocalHub::MAX_SUBSCRIPTIONS_PER_SESSION;

	for (size_t n = 0; n <= ceiling; n++)
	{
		char name[32] = {0,};
		snprintf(name, sizeof(name), "many%02zu", n);
		configureBoard(name, 15);
	}

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(30), "alice"));

	for (size_t n = 0; n < ceiling; n++)
	{
		char name[32] = {0,};
		snprintf(name, sizeof(name), "many%02zu", n);
		alice.send(std::string("HBBL BD") + name + " TS0");
	}

	pump([] { return false; }, 300);
	alice.forget();

	char last[32] = {0,};
	snprintf(last, sizeof(last), "many%02zu", ceiling);
	alice.send(std::string("HBBL BD") + last + " TS0");

	std::string sta;
	EXO_ASSERT(alice.waitForLine({"ISTA"}, sta));
	EXO_ASSERT(sta.find("170") != std::string::npos);
	EXO_ASSERT(sta.find(std::string("BD") + last) != std::string::npos);
	return 1;
});

/* And how often it may ask. */
EXO_TEST(bbs_hub_a_session_asks_at_a_bounded_rate,
{
	configureBoard("asking", 15);

	Server s;
	if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

	Client alice;
	EXO_ASSERT(alice.connect(s.port()));
	EXO_ASSERT(alice.login(makeIdentity(31), "alice"));

	for (unsigned n = 0; n < (unsigned) ADC::LocalHub::BOARD_REQUEST_BURST + 2; n++)
		alice.send("HBBL BDasking TS0");

	std::string sta;
	EXO_ASSERT(alice.waitForLine({"ISTA", "FCBBL"}, sta));
	EXO_ASSERT(sta.find("170") != std::string::npos);
	EXO_ASSERT(sta.find(" TL") != std::string::npos);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The index on disk                                                         */
/* ------------------------------------------------------------------------- */

/*
 * A board outlives the hub that indexed it, which is the point of a board: a user
 * who joins an hour late has missed everything said in chat, and a bulletin is
 * still there.
 *
 * The file is named after a hash of the board name and never after the name
 * itself, because a board name is protocol text and the character set permits '.'
 * - so '..' is a legal board name.
 */
EXO_TEST(bbs_hub_a_board_survives_the_hub_being_restarted,
{
	configureBoard("persistent", 15);

	const std::string tth = makeTTH(25);

	{
		Server s;
		if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

		Client alice;
		EXO_ASSERT(alice.connect(s.port()));
		EXO_ASSERT(alice.login(makeIdentity(32), "alice"));

		alice.send("HBBP TR" + tth + " SI412 BDpersistent SJWritten\\sdown");

		std::string line;
		EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + tth}, line));
	}

	/* A hub built again over the same directory, which is a restart in every way
	   that matters here. */
	{
		Server s;
		if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

		Client bob;
		EXO_ASSERT(bob.connect(s.port()));
		EXO_ASSERT(bob.login(makeIdentity(33), "bob"));

		/* The descriptor already says the board is not empty. */
		std::string bbd;
		EXO_ASSERT(bob.waitForLine({"IBBD", "BDpersistent", "NP1"}, bbd));

		bob.send("HBBL BDpersistent TS0");

		std::string line;
		EXO_ASSERT(bob.waitForLine({"IBBL", "TR" + tth}, line));
		EXO_ASSERT(fieldOf(line, "SI") == "412");
		EXO_ASSERT(fieldOf(line, "SJ") == "Written down");
		EXO_ASSERT(fieldOf(line, "NI") == "alice");
		EXO_ASSERT(fieldOf(line, "TH") == tth);

		/* And the post is still one post: submitting it again is a duplicate. */
		bob.send("HBBP TR" + tth + " SI412 BDpersistent SJAgain");

		std::string sta;
		EXO_ASSERT(bob.waitForLine({"ISTA"}, sta));
		EXO_ASSERT(sta.find("170") != std::string::npos);
	}
	return 1;
});

/*
 * An index file that has been truncated, or filled with something else, costs the
 * entries that cannot be read and nothing else.
 *
 * Every line is one whole ADC message read back with the reader a client reads a
 * BBL with, so a line that does not parse is skipped rather than repaired - and a
 * hub that fell over on one would be a hub any interrupted write could stop from
 * starting.
 */
EXO_TEST(bbs_hub_a_damaged_index_does_not_stop_the_hub,
{
	configureBoard("damaged", 15);

	const std::string good = makeTTH(26);

	{
		Server s;
		if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

		Client alice;
		EXO_ASSERT(alice.connect(s.port()));
		EXO_ASSERT(alice.login(makeIdentity(34), "alice"));

		alice.send("HBBP TR" + good + " SI100 BDdamaged SJSurvives");

		std::string line;
		EXO_ASSERT(alice.waitForLine({"IBBL", "TR" + good}, line));
	}

	/* Garbage before the entry, and a line cut off after it. */
	{
		ADC::LocalHub::BoardStore store(bbs);
		const std::string path = store.pathFor("damaged");
		EXO_ASSERT(!path.empty());

		Samurai::IO::File file(path);
		EXO_ASSERT(file.open(Samurai::IO::File::Mode::Read));

		const off_t size = file.size();
		std::string held(static_cast<size_t>(size), '\0');
		EXO_ASSERT(file.read(&held[0], held.size()) > 0);
		file.close();

		const std::string damaged =
			std::string("not a command at all\n")
			+ "IBBL BDdamaged\n"
			+ "IBBL TRnotahash BDdamaged TS1\n"
			+ held
			+ "IBBL TR" + makeTTH(27) + " SI100 BDdam";

		Samurai::IO::File out(path);
		EXO_ASSERT(out.open(Samurai::IO::File::Mode::Write
			| Samurai::IO::File::Mode::Truncate));
		EXO_ASSERT(out.write(damaged.data(), damaged.size()) > 0);
		out.close();
	}

	{
		Server s;
		if (!s.listen()) EXO_SKIP("could not listen on a loopback port");

		Client bob;
		EXO_ASSERT(bob.connect(s.port()));
		EXO_ASSERT(bob.login(makeIdentity(35), "bob"));

		std::string bbd;
		EXO_ASSERT(bob.waitForLine({"IBBD", "BDdamaged", "NP1"}, bbd));

		bob.send("HBBL BDdamaged TS0");

		std::string line;
		EXO_ASSERT(bob.waitForLine({"IBBL", "TR" + good}, line));
		EXO_ASSERT(fieldOf(line, "SJ") == "Survives");
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The hub owns the clock                                                    */
/* ------------------------------------------------------------------------- */

/**
 * The store with the clock in hand.
 *
 * A machine cannot be asked to move its clock backwards, and that is the case the
 * monotonicity rules exist for: an entry stamped before a second a client has
 * caught up to would be delivered to nobody, and no later entry could pull it back
 * into view because nothing enters the index in the past.
 */
class FrozenStore : public ADC::LocalHub::BoardStore
{
	public:
		explicit FrozenStore(const std::string& directory)
			: ADC::LocalHub::BoardStore(directory) { }

		uint64_t fake = 1000000;

	protected:
		uint64_t now() const override { return fake; }
};

EXO_TEST(bbs_hub_a_clock_that_moves_backwards_does_not_move_the_board_back,
{
	configureBoard("clock", 15);

	FrozenStore store(bbs);
	store.readConfiguration(core->config);
	EXO_ASSERT(store.isUsable());
	EXO_ASSERT(store.exists("clock"));

	const Identity author = makeIdentity(36);

	QuickDC::BoardEntry post;
	post.cid = author.cid;
	post.nick = "alice";
	post.size = 100;

	QuickDC::BoardEntry first;
	post.tth = makeTTH(28);
	EXO_ASSERT(store.add("clock", post, first)
		== ADC::LocalHub::BoardStore::Outcome::Accepted);
	EXO_ASSERT_EQ_UINT(first.timestamp, 1000000u);

	/* The clock moves backwards, by an hour. */
	store.fake = 1000000 - 3600;

	QuickDC::BoardEntry second;
	post.tth = makeTTH(29);
	EXO_ASSERT(store.add("clock", post, second)
		== ADC::LocalHub::BoardStore::Outcome::Accepted);

	/* The highest already assigned is reused rather than anything lower. */
	EXO_ASSERT_EQ_UINT(second.timestamp, first.timestamp);

	/* And a withdrawal is still later than the entry it replaces, however far back
	   the clock has gone: a client ignores an arriving entry for a hash it holds
	   unless the timestamp has moved on. */
	QuickDC::BoardEntry stone;
	EXO_ASSERT(store.withdraw("clock", makeTTH(29), stone));
	EXO_ASSERT(stone.timestamp > second.timestamp);
	EXO_ASSERT(stone.removed);

	/* Nothing entered the index in the past, so the entries are still in the order
	   a subscriber may be sent them. */
	const std::vector<QuickDC::BoardEntry> entries = store.since("clock", 0);
	EXO_ASSERT(entries.size() == 2);
	for (size_t n = 1; n < entries.size(); n++)
		EXO_ASSERT(entries[n - 1].timestamp <= entries[n].timestamp);

	/* And the forward clock is where a later post lands. */
	store.fake = 1000000 + 60;

	QuickDC::BoardEntry third;
	post.tth = makeTTH(32);
	EXO_ASSERT(store.add("clock", post, third)
		== ADC::LocalHub::BoardStore::Outcome::Accepted);
	EXO_ASSERT_EQ_UINT(third.timestamp, 1000060u);
	return 1;
});

/*
 * What one author may hold on one board.
 *
 * Refused rather than made room for: dropping this author's oldest post to fit
 * their newest would make posting a way to unpublish, and it would leave every
 * client holding an entry the hub had quietly forgotten - where a withdrawal is a
 * tombstone that reaches them.
 */
EXO_TEST(bbs_hub_an_author_holds_a_bounded_number_of_entries,
{
	configureBoard("quota", 15, 262144, 512, 2);

	FrozenStore store(bbs);
	store.readConfiguration(core->config);

	QuickDC::BoardEntry post;
	post.cid = makeIdentity(37).cid;
	post.size = 100;

	QuickDC::BoardEntry out;

	post.tth = makeTTH(33);
	EXO_ASSERT(store.add("quota", post, out)
		== ADC::LocalHub::BoardStore::Outcome::Accepted);

	post.tth = makeTTH(34);
	EXO_ASSERT(store.add("quota", post, out)
		== ADC::LocalHub::BoardStore::Outcome::Accepted);

	post.tth = makeTTH(35);
	EXO_ASSERT(store.add("quota", post, out)
		== ADC::LocalHub::BoardStore::Outcome::AuthorLimit);

	/* Somebody else is not held to what this author has spent. */
	post.cid = makeIdentity(39).cid;
	EXO_ASSERT(store.add("quota", post, out)
		== ADC::LocalHub::BoardStore::Outcome::Accepted);

	/* And withdrawing one makes room, which is the sanctioned way for an entry to
	   leave a board. */
	QuickDC::BoardEntry stone;
	EXO_ASSERT(store.withdraw("quota", makeTTH(33), stone));

	post.cid = makeIdentity(37).cid;
	post.tth = makeTTH(36);
	EXO_ASSERT(store.add("quota", post, out)
		== ADC::LocalHub::BoardStore::Outcome::Accepted);
	return 1;
});

/*
 * What a board holds, and what it says about what it has dropped.
 *
 * The index is an unbounded structure that remote users write to, so a board is
 * bounded - and OT is where a hub that expires its oldest entries says so, which is
 * the only warning a client with an older cursor gets that what it holds has a gap
 * no subscription will close.
 */
EXO_TEST(bbs_hub_a_full_board_drops_its_oldest_and_says_so,
{
	configureBoard("bounded", 15, 262144, 3);

	FrozenStore store(bbs);
	store.readConfiguration(core->config);

	QuickDC::BoardEntry post;
	post.cid = makeIdentity(36).cid;
	post.size = 100;

	QuickDC::Board described;
	EXO_ASSERT(store.describe("bounded", ADC::LocalHub::Cred_User, described));
	EXO_ASSERT_EQ_UINT(described.oldest, 0u);

	std::vector<uint64_t> stamps;

	for (unsigned n = 0; n < 5; n++)
	{
		QuickDC::BoardEntry out;
		post.tth = makeTTH(100 + n);
		store.fake = 2000000 + n;
		EXO_ASSERT(store.add("bounded", post, out)
			== ADC::LocalHub::BoardStore::Outcome::Accepted);
		stamps.push_back(out.timestamp);
	}

	const std::vector<QuickDC::BoardEntry> held = store.since("bounded", 0);
	EXO_ASSERT_EQ_UINT(held.size(), 3u);

	/* The newest three, and the oldest two are gone. */
	EXO_ASSERT(held.front().tth == makeTTH(102));
	EXO_ASSERT(held.back().tth == makeTTH(104));

	EXO_ASSERT(store.describe("bounded", ADC::LocalHub::Cred_User, described));
	EXO_ASSERT_EQ_UINT(described.oldest, stamps[2]);
	EXO_ASSERT_EQ_UINT(described.newest, stamps[4]);
	EXO_ASSERT_EQ_UINT(described.posts, 3u);

	/* A request from before that is a request from there, rather than a refusal:
	   the hub declared how far back it would go. */
	const std::vector<QuickDC::BoardEntry> replay = store.since("bounded", 1);
	EXO_ASSERT_EQ_UINT(replay.size(), 3u);
	return 1;
});
