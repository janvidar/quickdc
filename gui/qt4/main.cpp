/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <qapplication.h>
#include <qeventloop.h>
#include <qwidget.h>
#include <qtimer.h>

#include <api/core.h>
#include <debug/dbg.h>
#include <samurai/io/net/url.h>
#include "mainview.h"
#include "scheduler.h"

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	CoreScheduler core;
	
	Chrome::Qt4::MainView gui(core);
	

	{
		Samurai::IO::Net::URL url("dchub://dev.myhub.org:1416");
		gui.addHub(&url);
	}

/*
	{
		Samurai::IO::Net::URL url("adc://localhost:1411");
		gui.addHub(&url);
	}
*/

	gui.show();
	return app.exec();
}
