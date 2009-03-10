/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CORE_API_H
#define HAVE_QUICKDC_CORE_API_H

#include <samurai/timestamp.h>

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
	class Hub;
}

namespace QuickDC {
namespace Hash { class Manager; }
namespace Share { class Manager; }

class ConnectionManager;
class DCCommandProxy;
class DCHubSession;
class DCServer;
class HubManager;
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
		QuickDC::Share::Manager* shares;
		DCServer* server;
		ConnectionManager* connections;
		HubManager* hubs;
		QuickDC::Hash::Manager* hash;
		TransferManager* transfers;
		Samurai::TimerManager* timers;
		ADC::Hub* localHub;
		QuickDC::QueueManager* queue;
		Samurai::TimeStamp startup;
		Samurai::MessageHandler* messageHandler;
		Samurai::IO::Net::SocketMonitor* monitor;
		QuickDC::SecurityManager* securityManager;
		Samurai::IO::Net::BandwidthManager* bandwidthManager;

	protected:
		int references;

	public:
		/**
		 * Note: only one Core object can exist
		 * Constructing multiple will throw an error.
		 */
		Core();
		virtual ~Core();
};

}

extern "C" QuickDC::Core* quickdc_core;

#endif // HAVE_QUICKDC_CORE_API_H
