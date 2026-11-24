/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CORE_API_H
#define HAVE_QUICKDC_CORE_API_H

#include <samurai/eventloop.h>
#include <samurai/timestamp.h>

#include "api/inbox.h"
#include "api/protocol.h"
#include "api/subscription.h"

#include <atomic>
#include <functional>
#include <string>

/* The account's own home: where Core() keeps everything it reads and writes. */
#define QUICKDC_HOME              "~/.quickdc"

/*
 * Where this client keeps the certificate it presents to peers, and the name that
 * certificate asserts. Named here rather than written into core.cpp because the
 * generator refuses a name it cannot encode - so a label that stops being usable
 * has to fail somewhere a test can see it, not by quietly leaving the client
 * without a certificate. A core over another home keeps the same names under it.
 */
#define QUICKDC_PRIVATE_KEY_FILE  QUICKDC_HOME "/ssl/quickdc.key"
#define QUICKDC_CERTIFICATE_FILE  QUICKDC_HOME "/ssl/quickdc.crt"
#define QUICKDC_CERTIFICATE_NAME  "quickdc"

/* Where the chat transcripts live; one file per conversation. */
#define QUICKDC_CHAT_DIR          QUICKDC_HOME "/chat"

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
	
	class Timer;
}


namespace ADC {
	class CID;
	namespace LocalHub { class Hub; }
}

namespace QuickDC {
namespace Hash { class Manager; }
namespace Share { class Manager; }

class ConnectionManager;
class DCCommandProxy;
class DCHubSession;
class DCServer;
class HubManager;
class DropCheck;
class PortForward;
class Reachability;
class SearchTokens;
class Preferences;
class Server;
class ShareManager;
class TransferManager;
class ChatArchive;
class QueueManager;
class SecurityManager;

namespace Api { class Service; }

class Core {
	public:
		/**
		 * Run the core for a while.
		 * This needs to be run often in a loop, in order to 
		 * keep the networking going and messages dispatched.
		 *
		 * QuickDC core is asynchronous so calling this will
		 * not block your application for a long time. Usually only
		 * for milliseconds.
		 *
		 * In order to get good networking throughput, you should 
		 * call this method at least 20 times per second.
		 *
		 * This call will never block for a considerable long time.
		 */
		void run();

		/**
		 * Drive the core on this thread until requestStop().
		 *
		 * Opens the inbox, calls 'running' once jobs are being accepted, and
		 * runs the loop and the inbox in turn. Returns once a stop has been
		 * asked for, having run every job queued before it was.
		 */
		void runUntilStopped(const std::function<void()>& running = {});

		/** Have runUntilStopped() return. Any thread, any number of times. */
		void requestStop();

		/**
		 * Run 'job' on the thread in runUntilStopped(). Any thread.
		 *
		 * @return false where the core is not being run that way, or has
		 *         stopped; the job is then destroyed unrun, here.
		 */
		bool post(Inbox::Job job) { return inbox.post(std::move(job)); }

		/**
		 * Carry out 'command' on the core's thread. Any thread.
		 *
		 * Through the inbox, so only while runUntilStopped() is running; what
		 * comes of it is told to the event sinks.
		 *
		 * @return false where the command was refused unrun.
		 */
		bool submit(Api::Command command);

		/** Both preference stores as they stand. Core thread only. */
		Api::SettingsSnapshot settingsSnapshot() const;

		/** Tell 'sink' every event until the subscription ends. Core thread only. */
		Subscription subscribe(Api::EventSink* sink);
		
#if 0
		/**
		 * Add a server listener. This will be notified 
		 * whenever the server receivs a connection.
		 */
		 void AddServerListener(const DCServerListener*);
#endif // 0
		
		
	public:
		/**
		 * Returns the version string of the compiled QuickDC core.
		 */
		const char* getVersion() const;

		/**
		 * What every socket, timer, message and lookup of this core is made on.
		 *
		 * Made before anything else the core holds and gone after all of it, so
		 * nothing a core makes outlives the loop it runs on. Core thread only.
		 */
		Samurai::EventLoop& loop() { return eventLoop; }
		
	public:
		Preferences* config;

		/**
		 * Per-hub session data: nickname, password, last connect time, and the
		 * keyprint remembered for a hub.
		 *
		 * One store for the whole process, shared by every hub session, because
		 * they all persist to the same file. A Preferences holds the entire file
		 * in memory and writes all of it back, so a store per session would write
		 * a snapshot taken before the others existed and silently drop their
		 * settings, a remembered keyprint among them. Sessions borrow this and
		 * must not delete it.
		 *
		 * The current group is shared state as a result: select a group
		 * immediately before reading or writing, never rely on one set earlier.
		 */
		Preferences* session;

		/**
		 * This client's identity, read from 'config' or made and stored there.
		 * Null in a core built without one, where nothing announces itself.
		 */
		ADC::CID* identity;

		/** Network state the core acts on by itself; see api/core.cpp. */
		class Automation;
		Automation* automation;

		/** Where submitted commands are carried out; see api/service.h. */
		Api::Service* service;

		QuickDC::Share::Manager* shares;
		DCServer* server;
		ConnectionManager* connections;
		HubManager* hubs;
		QuickDC::Hash::Manager* hash;
		TransferManager* transfers;
		ADC::LocalHub::Hub* localHub;
		QuickDC::QueueManager* queue;

		/**
		 * What was said on every hub, so a conversation outlives the session it
		 * happened in.
		 *
		 * Written as the chat arrives rather than by whoever is displaying it: a
		 * transcript is the client's record of the hub, and a client with no
		 * window open on a conversation is still in it. Null in a core built
		 * without one, which every caller checks for as it does for the queue.
		 */
		QuickDC::ChatArchive* chat;

		/**
		 * Asks the internet gateway to forward the peer port, or null when the
		 * core was built without a listening socket to forward to.
		 *
		 * Started here only when [Network] UseUPnP says so. A front end is free to
		 * drive it instead - start() it, stop() it, and read isForwarded(),
		 * getExternalPort() and getExternalAddress() to show what state it is in -
		 * which is why this is reachable rather than private.
		 */
		PortForward* portforward;
		Samurai::TimeStamp startup;
		QuickDC::SecurityManager* securityManager;

		/**
		 * What is known about being reachable from outside.
		 *
		 * Here rather than on a hub session because it is a property of this
		 * client's own port, not of any one hub: a connection that arrives is
		 * evidence whichever hub sent the peer, and a front end asking whether
		 * this client is reachable is asking one question, not one per hub.
		 */
		QuickDC::Reachability* reachability;

		/**
		 * The same for the IPv6 endpoint, which is a different endpoint.
		 *
		 * One record answers for one advertised address and port, and the two
		 * families are two of those: a client can be reached natively over IPv6
		 * while every IPv4 attempt dies in a carrier-grade NAT, and either
		 * record standing in for the other would report one family's luck as
		 * the other's. Kept apart for the same reason Reachability discards
		 * everything when the endpoint changes.
		 */
		QuickDC::Reachability* reachability6;

		/**
		 * Whichever of the two answers for 'ipv6'.
		 *
		 * Here so that a caller holding an address decides once, at the point it
		 * knows the family, rather than every site picking a member by name and
		 * one of them picking wrong.
		 */
		QuickDC::Reachability* reachabilityFor(bool ipv6) const
		{
			return ipv6 ? reachability6 : reachability;
		}

		/**
		 * What a DROP server measured about being reachable from outside.
		 *
		 * Beside Reachability rather than inside it because the two answer the
		 * same question from opposite ends: that one waits for evidence to
		 * arrive by itself and can wait for ever, while this asks for it and
		 * gets an answer or a reason there is none. Null unless a server was
		 * configured to ask - the request tells that server this client's
		 * address and the port it listens on and has it connect back to prove
		 * the answer, which is a disclosure a user chooses rather than one this
		 * makes for them, so a measurement is taken because somebody asked for
		 * it and not because the program started.
		 *
		 * Destroyed before Core::portforward, since a run in flight holds a
		 * socket the monitor is still walking and stops answering as it goes.
		 */
		QuickDC::DropCheck* dropcheck;

		/**
		 * Takes the measurement again once the one in hand is too old to believe.
		 *
		 * Present whenever there is a port to measure, including where no server
		 * was configured to ask: one named later is measured without a restart,
		 * and this is what keeps measuring it. It asks nobody anything until
		 * there is a server, and then at most once per
		 * QUICKDC_REACHABILITY_MEASUREMENT_AGE - the disclosure is the one
		 * already chosen by naming that server, repeated for as long as the
		 * client runs, and a measurement nothing refreshes is worth having only
		 * for the first quarter of an hour of a session.
		 *
		 * Null in a core built without a listening socket, as dropcheck is.
		 * Destroyed before Core::dropcheck, which is what it starts.
		 */
		Samurai::Timer* dropRefresh;

		/**
		 * The searches sent, so a datagram answering one can be told from a
		 * stranger's. Feeds the reachability record above.
		 */
		QuickDC::SearchTokens* searchTokens;

	protected:
		/* First, so it is the last thing destroyed: the inbox below holds a socket on it. */
		Samurai::EventLoop eventLoop;

		int references;

		Inbox inbox;
		std::atomic<bool> stopRequested{false};

	public:
		/** The directory a full core keeps everything it reads and writes under. */
		struct Home {
			std::string directory;

			/**
			 * Settings to take as though the two files held them, before anything
			 * reads either - so what the core does as it starts, such as which port
			 * it binds and whom it asks about reachability, can be decided for it.
			 */
			Api::SettingsGroups settings;
			Api::SettingsGroups session;
		};

		/** The client proper, over QUICKDC_HOME. */
		Core();

		/**
		 * The client proper, over 'home': its configuration, session, queue,
		 * transcripts, file lists, certificate and local hub's files are all
		 * under that directory, which is made if it is absent.
		 *
		 * More than one can be built. Each runs on an event loop of its own, so
		 * two in one process share no socket, timer or message, and each can be
		 * driven from a thread of its own. What they do share is the TLS keys,
		 * which are set for the whole process: the last core built presents its
		 * certificate for all of them, and building two at once races on it.
		 */
		explicit Core(const Home& home);

		/**
		 * A core that runs a hub session and nothing else.
		 *
		 * The full constructor creates ~/.quickdc, reads and writes the user's
		 * configuration, scans the shares and opens a listening socket, none of
		 * which a hub session needs. This builds the four things it does need -
		 * the two preference stores, the connection manager and the hub manager
		 * - with the stores taken as arguments so they can be pointed away from
		 * the real ones, plus a security manager and a transfer manager, which
		 * peer sessions consult without checking for either. A download queue is built only if a file is
		 * named for it; the sessions that use one check first. Every other
		 * member is null.
		 *
		 * @param chat_dir where to keep the chat transcripts, or empty for a core
		 *        that keeps none. Empty by default for the same reason the queue
		 *        is: a core built to stay out of the user's files must not start
		 *        writing their conversations into them.
		 */
		Core(const std::string& config_file, const std::string& session_file,
		     const std::string& queue_file = std::string(),
		     const std::string& chat_dir = std::string());

		virtual ~Core();

		Core(const Core&) = delete;
		Core& operator=(const Core&) = delete;
};

}

#endif // HAVE_QUICKDC_CORE_API_H
