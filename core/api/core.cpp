/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include <samurai/io/net/bandwidth.h>
#include <samurai/io/net/tlsfactory.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/file.h>
#include <samurai/timer.h>
#include <samurai/timestamp.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "api/core.h"
#include "api/server.h"
#include "config/preferences.h"
#include "network/connectionmanager.h"
#include "network/downloadqueue.h"
#include "network/hubmanager.h"
#include "network/securitymanager.h"
#include "network/transfermanager.h"
#include "network/adc/server/adchub.h"

#include "share/sharemanager.h"
#include "hash/hashmanager.h"

extern "C" QuickDC::Core* quickdc_core;

QuickDC::Core::Core()
{
	QDBG_INIT;

	startup = time(0);

	references = 0;
	
	Samurai::IO::File::mkdir("~/.quickdc",         0700);
	Samurai::IO::File::mkdir("~/.quickdc/hub",     0700);
	Samurai::IO::File::mkdir("~/.quickdc/hub/bbs", 0700);
	Samurai::IO::File::mkdir("~/.quickdc/lists",   0700);
	Samurai::IO::File::mkdir("~/.quickdc/ssl",     0700);
	
	config = new Preferences("~/.quickdc/quickdc.conf");
	quickdc_core = this;
	
	monitor = Samurai::IO::Net::SocketMonitor::getInstance();
	bandwidthManager = new Samurai::IO::Net::BandwidthManager();
	messageHandler = Samurai::MessageHandler::getInstance();
	securityManager = new QuickDC::SecurityManager();

	Samurai::IO::Net::TlsFactory::setKeys("~/.quickdc/ssl/quickdc.key", "~/.quickdc/ssl/quickdc.crt");

	hash = new QuickDC::Hash::Manager();
	shares = new QuickDC::Share::Manager(config, hash);
	server = new DCServer();
	connections = new ConnectionManager();
	hubs = new HubManager();
	transfers = new TransferManager();
	timers = new Samurai::TimerManager();
	localHub = new ADC::Hub();
	queue = new QuickDC::QueueManager();
	
	shares->initialize();
}


QuickDC::Core::~Core()
{
	quickdc_core = 0;
	delete timers; timers = 0;
	delete transfers; transfers = 0;
	delete connections; connections = 0;
	delete server; server = 0;
	delete hubs; hubs = 0;
	delete securityManager; securityManager = 0;
	delete shares; shares = 0;
	delete config; config = 0;
	delete hash; hash = 0;
	delete localHub; localHub = 0;
	delete bandwidthManager;
	delete messageHandler; messageHandler = 0;
	QDBG_FINI;
}


void QuickDC::Core::run()
{
	// Poll sockets. A zero timeout turns the caller's main loop into a spin -
	// kevent/poll returns immediately and we burn a core doing nothing. Block
	// briefly instead; callers that also poll a non-socket input source (the
	// CLI polls /dev/tty) see at most this much added latency.
	//
	// While hashing is in progress we must not block at all: hash->process()
	// below only advances one chunk per call, so any wait here throttles
	// hashing to 1000/POLL_INTERVAL_MS chunks per second.
	// FIXME: register the other input sources with the monitor, and drive
	// hashing off a worker thread, so this can block indefinitely.
	static const int POLL_INTERVAL_MS = 50;
	monitor->wait(hash->isBusy() ? 0 : POLL_INTERVAL_MS);

	// Make sure hashing is continued - we should post messages instead.
	hash->process();
	
	// Process timers.
	// FIXME: it should be sorted so we don't have to iterate ALL timers to before exit.
	timers->process();
	messageHandler->process();
}


QuickDC::Core* QuickDC::Core::getInstance()
{
	return quickdc_core;
}


const char* QuickDC::Core::getVersion() const 
{
	return PRODUCT " " VERSION " " SYSTEM "/" CPU
#ifdef BUILD
	 " (build " BUILD ")"
#endif
	 ;
}


