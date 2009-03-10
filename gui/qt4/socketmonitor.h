/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_GUI_QT4_SOCKETMONITOR_H
#define HAVE_QUICKDC_GUI_QT4_SOCKETMONITOR_H

#include <vector>
#include <api/core.h>
#include <samurai/io/net/socketmonitor.h>
#include <QObject>

class QSocketNotifier;
class QtSocketStorage;
class CoreScheduler;

class QtSocketMonitor : public Samurai::IO::Net::SocketMonitor
{
	public:
		QtSocketMonitor(CoreScheduler& scheduler);
		virtual ~QtSocketMonitor();
		
		void internal_add(Samurai::IO::Net::SocketBase* socket);
		void internal_remove(Samurai::IO::Net::SocketBase* socket);
		void internal_modify(Samurai::IO::Net::SocketBase* socket);
		void wait(int time_ms);
		size_t size();
		size_t capacity();
		bool isValid();
		
	private:
		std::vector<QtSocketStorage*> sockets;
		CoreScheduler& scheduler;
};

class QtSocketStorage : public ::QObject
{
	Q_OBJECT
	
	public:
		QtSocketStorage(Samurai::IO::Net::SocketBase* sock, CoreScheduler& scheduler);
		virtual ~QtSocketStorage();
		
		int getFD() const { return sd; }
		int getTriggered() const { return triggered; }
		void resetTrigger();
		void updateTrigger();
		Samurai::IO::Net::SocketBase* getSocket() const { return socket; }
		
	private slots:
		void triggerRead(int);
		void triggerWrite(int);
		void triggerException(int);
		
	private:
		int sd;
		int triggered;
		Samurai::IO::Net::SocketBase* socket;
		QSocketNotifier* rx_notifier;
		QSocketNotifier* tx_notifier;
		QSocketNotifier* ex_notifier;
		CoreScheduler& scheduler;
};


#endif // HAVE_QUICKDC_GUI_QT4_SOCKETMONITOR_H
