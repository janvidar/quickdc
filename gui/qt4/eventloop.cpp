/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <qeventloop.h>
#include "eventloop.h"
#include <api/core.h>
#include <debug/dbg.h>

Chrome::Qt4::EventLoop::EventLoop() {
//	core = new QuickDC::DCCore();
	QDBG("Custom Qt3 event loop created...");
}
	
