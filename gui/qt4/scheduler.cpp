/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <samurai/samurai.h>
#include "scheduler.h"
#include "socketmonitor.h"
#include <QTimer>

CoreScheduler::CoreScheduler()
{
	monitor = new QtSocketMonitor(*this);
	
	core = new QuickDC::Core();
	timer = new QTimer();
	timer->setSingleShot(true);
	connect(timer, SIGNAL(timeout()), this, SLOT(run()));
}

CoreScheduler::~CoreScheduler()
{
	delete timer;
	delete core;
	delete monitor;
}

void CoreScheduler::run()
{
	core->run();
}

void CoreScheduler::reschedule(size_t ms)
{
	timer->start(ms);
}