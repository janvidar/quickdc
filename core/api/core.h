/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CORE_API_H
#define HAVE_QUICKDC_CORE_API_H

#include <samurai/timestamp.h>

/*
 * Where this client keeps the certificate it presents to peers, and the name that
 * certificate asserts. Named here rather than written into core.cpp because the
 * generator refuses a name it cannot encode - so a label that stops being usable
 * has to fail somewhere a test can see it, not by quietly leaving the client
 * without a certificate.
 */
#define QUICKDC_PRIVATE_KEY_FILE  "~/.quickdc/ssl/quickdc.key"
#define QUICKDC_CERTIFICATE_FILE  "~/.quickdc/ssl/quickdc.crt"
#define QUICKDC_CERTIFICATE_NAME  "quickdc"

namespace Samurai {
	namespace IO {
		namespace Net {
			class BandwidthManager;
			class SocketMonitor;
			class Socket;
		}
	}
	
	class TimerManager;
	class MessageHandler;
}


namespace ADC {
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
class PortForward;
class Reachability;
class SearchTokens;
class Preferences;
class Server;
class ShareManager;
class TransferManager;
class QueueManager;
class SecurityManager;

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
		 * Returns an instance of the running Core, 
		 * or it will create one if none is running.
		 */
		static Core* getInstance();
		
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

		QuickDC::Share::Manager* shares;
		DCServer* server;
		ConnectionManager* connections;
		HubManager* hubs;
		QuickDC::Hash::Manager* hash;
		TransferManager* transfers;
		Samurai::TimerManager* timers;
		ADC::LocalHub::Hub* localHub;
		QuickDC::QueueManager* queue;

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
		Samurai::MessageHandler* messageHandler;
		Samurai::IO::Net::SocketMonitor* monitor;
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
		 * The searches sent, so a datagram answering one can be told from a
		 * stranger's. Feeds the reachability record above.
		 */
		QuickDC::SearchTokens* searchTokens;
		Samurai::IO::Net::BandwidthManager* bandwidthManager;

	protected:
		int references;

	public:
		/**
		 * Note: only one Core object can exist
		 * Constructing multiple will throw an error.
		 */
		Core();

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
		 */
		Core(const std::string& config_file, const std::string& session_file,
		     const std::string& queue_file = std::string());

		virtual ~Core();

		Core(const Core&) = delete;
		Core& operator=(const Core&) = delete;
};

}

/*
 * NOTE: not extern "C". The definition in api/server.cpp is not either, and
 * MSVC mangles a global variable's name, so the two have to agree.
 */
extern QuickDC::Core* quickdc_core;

#endif // HAVE_QUICKDC_CORE_API_H
