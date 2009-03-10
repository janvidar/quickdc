/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_GUI_QT4_EVENTLOOP_H
#define HAVE_QUICKDC_GUI_QT4_EVENTLOOP_H

#include <QAbstractEventDispatcher>

namespace QuickDC {
class DCCore;
}


namespace Chrome {
namespace Qt4 {

class EventLoop : public QAbstractEventDispatcher {

	public:
		EventLoop();
#if 0
		bool processEvents(QEventLoop::ProcessEventsFlags flags);
		bool hasPendingEvents() const;

	public slots:
		void service();
	protected:
		QuickDC::DCCore* core;
#endif
};

}
}

#endif // HAVE_QUICKDC_GUI_QT4_EVENTLOOP_H

