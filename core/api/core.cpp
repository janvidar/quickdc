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
	/* The default download destination; [Downloads]/Directory overrides it. */
	Samurai::IO::File::mkdir(QUICKDC_DOWNLOAD_DIR, 0700);
	
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
	/* Flush the download queue before anything it might refer to goes away. */
	delete queue; queue = 0;
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
	// Poll sockets. A zero timeout turns this into a spin - kevent/poll returns
	// immediately and we burn a core doing nothing - so always block.
	//
	// Neither of the two things that used to force a short timeout applies any
	// more: hashing runs on its own thread, and console input reaches us
	// through a socket the monitor watches, so a keypress wakes the loop
	// immediately rather than on the next tick. What is left is what is still
	// polled rather than event driven, and that sets the interval: collecting
	// finished hash jobs, and timers, which are all whole seconds anyway.
	static const int POLL_INTERVAL_MS = 250;
	monitor->wait(POLL_INTERVAL_MS);

	// Collect whatever the hash worker thread has finished.
	hash->process();

	// Announce transfers that have finished.
	//
	// A transfer reports completion from inside its own exec(), and the session
	// that owns it responds by deleting it, so the notification has to be
	// deferred to here rather than made on the spot. TransferManager parks them
	// on a list for exactly that reason; nothing was draining the list, which
	// left a finished download sitting with its socket open waiting for data
	// that was never coming.
	transfers->process();

	// Expire dead sources, start queued transfers, and flush the queue now
	// and then. Cheap enough to run on every iteration.
	queue->process();

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


