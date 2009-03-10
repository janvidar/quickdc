/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_GUI_QT4_CORE_SCHEDULER_H
#define HAVE_QUICKDC_GUI_QT4_CORE_SCHEDULER_H

#include <api/core.h>
#include <QObject>

class QTimer;
class QtSocketMonitor;

class CoreScheduler : public QObject
{
	Q_OBJECT
	
	public:
		CoreScheduler();
		virtual ~CoreScheduler();
		
		void reschedule(size_t ms = 0);
		
	public slots:
		void run();
		
	protected:
		QtSocketMonitor* monitor;
		QuickDC::Core* core;
		QTimer* timer;
};


#endif // HAVE_QUICKDC_GUI_QT4_CORE_SCHEDULER_H
