/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_GUI_QT4_MAINVIEW_H
#define HAVE_QUICKDC_GUI_QT4_MAINVIEW_H

#include <QMainWindow>
#include "widgetlist.h"
#include "socketmonitor.h"
#include "scheduler.h"

#include <api/core.h>

#include <samurai/io/net/url.h>


class QListWidget;
class QWidget;
class QStackedWidget;
class CoreScheduler;

namespace Chrome {
namespace Qt4 {

class MainView : public QMainWindow
{
    Q_OBJECT
	
	public:
		MainView(CoreScheduler&);
		~MainView();
		
		void addHub(Samurai::IO::Net::URL* url);

	protected slots: /* Window handling */
		void activateWindow(QWidget*);
	
	protected:
		void initialize();
		void shutdown();
		void addWidget(QWidget*);
		
	protected:
		WidgetList*     switcher;
		QStackedWidget* windows;
		CoreScheduler& core;
};

}
}
#endif // HAVE_QUICKDC_GUI_QT4_MAINVIEW_H
