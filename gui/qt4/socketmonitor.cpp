/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <samurai/samurai.h>
#include <samurai/io/net/socketbase.h>
#include "socketmonitor.h"
#include "scheduler.h"
#include <QSocketNotifier>

// #define DEBUG_SOCKETMONITOR

QtSocketStorage::QtSocketStorage(Samurai::IO::Net::SocketBase* sock, CoreScheduler& sched) : sd(sock->getFD()), triggered(0), socket(sock), rx_notifier(0), tx_notifier(0), scheduler(sched)
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("Socket: QtSocketStorage(): sock=%p, sd=%d", sock, sd);
#endif
	rx_notifier = new QSocketNotifier(sd, QSocketNotifier::Read);
	tx_notifier = new QSocketNotifier(sd, QSocketNotifier::Write);
	ex_notifier = new QSocketNotifier(sd, QSocketNotifier::Exception);

	connect(rx_notifier, SIGNAL(activated(int)), this, SLOT(triggerRead(int)));
	connect(tx_notifier, SIGNAL(activated(int)), this, SLOT(triggerWrite(int)));
	connect(ex_notifier, SIGNAL(activated(int)), this, SLOT(triggerException(int)));
	
	updateTrigger();
}

QtSocketStorage::~QtSocketStorage()
{
	delete rx_notifier;
	delete tx_notifier;
}

void QtSocketStorage::triggerRead(int)
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("QtSocketStorage::triggerRead(): sock=%p, sd=%d", socket, sd);
#endif
	triggered |= Samurai::IO::Net::SocketMonitor::MRead;
	scheduler.reschedule();
}

void QtSocketStorage::triggerWrite(int)
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("QtSocketStorage::triggerWrite(): sock=%p, sd=%d", socket, sd);
#endif
	triggered |= Samurai::IO::Net::SocketMonitor::MWrite;
	scheduler.reschedule();
}

void QtSocketStorage::triggerException(int)
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("QtSocketStorage::triggerException(): sock=%p, sd=%d", socket, sd);
#endif
	triggered |= Samurai::IO::Net::SocketMonitor::MRead;
	scheduler.reschedule();
}


void QtSocketStorage::resetTrigger()
{
	triggered = 0;
}

void QtSocketStorage::updateTrigger()
{
	tx_notifier->setEnabled(socket->getMonitorTrigger() & Samurai::IO::Net::SocketMonitor::MWrite);
	rx_notifier->setEnabled(socket->getMonitorTrigger() & Samurai::IO::Net::SocketMonitor::MRead);
}

QtSocketMonitor::QtSocketMonitor(CoreScheduler& sched) : Samurai::IO::Net::SocketMonitor("qt4"), scheduler(sched)
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("Socket: %s", __PRETTY_FUNCTION__);
#endif
	Samurai::IO::Net::SocketMonitor::setSocketMonitor(this);
}

QtSocketMonitor::~QtSocketMonitor()
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("Socket: %s", __PRETTY_FUNCTION__);
#endif
	Samurai::IO::Net::SocketMonitor::setSocketMonitor(0);
}


void QtSocketMonitor::internal_add(Samurai::IO::Net::SocketBase* socket)
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("Socket: %s", __PRETTY_FUNCTION__);
#endif
	QtSocketStorage* sock = new QtSocketStorage(socket, scheduler);
	sockets.push_back(sock);
}


void QtSocketMonitor::internal_remove(Samurai::IO::Net::SocketBase* socket)
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("Socket: %s", __PRETTY_FUNCTION__);
#endif
	std::vector<QtSocketStorage*>::iterator it;
	for (it = sockets.begin(); it != sockets.end(); it++)
	{
		QtSocketStorage* sock = *it;
		if (sock->getFD() == socket->getFD())
		{
			sockets.erase(it);
			delete sock;
			break;
		}
	}
}


void QtSocketMonitor::internal_modify(Samurai::IO::Net::SocketBase* socket)
{
	// qDebug("Socket: %s %p", __PRETTY_FUNCTION__, socket);
	std::vector<QtSocketStorage*>::iterator it;
	for (it = sockets.begin(); it != sockets.end(); it++)
	{
		QtSocketStorage* sock = (*it);
		if (sock->getFD() == socket->getFD())
		{
			sock->updateTrigger();
			break;
		}
	}
}
struct poll_act
{
	Samurai::IO::Net::SocketBase* sock;
	int trig;
};

void QtSocketMonitor::wait(int)
{
//	qDebug("Socket: %s", __PRETTY_FUNCTION__);

	struct poll_act* act = new struct poll_act[sockets.size()];
	size_t act_num = 0;
	
	std::vector<QtSocketStorage*>::iterator it = sockets.begin();
	for (; it != sockets.end(); it++)
	{
		QtSocketStorage* sock = (*it);
		if (sock->getTriggered())
		{
			act[act_num].sock = sock->getSocket();
			act[act_num].trig = sock->getTriggered();
			act_num++;
			sock->resetTrigger();
		}
	}
	
	for (size_t n = 0; n < act_num; n++)
	{
#ifdef DEBUG_SOCKETMONITOR
		qDebug("handleSocketEvent, n=%zu/%zu, sock=%p, trig=%x", n, act_num, act[n].sock, act[n].trig);
#endif
		handleSocketEvent(act[n].sock, act[n].trig);
	}
	
	delete[] act;
}


size_t QtSocketMonitor::size()
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("Socket: %s", __PRETTY_FUNCTION__);
#endif
	return sockets.size();
}


size_t QtSocketMonitor::capacity()
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("Socket: %s", __PRETTY_FUNCTION__);
#endif
	// FIXME: Implement me!
	return 64;
}


bool QtSocketMonitor::isValid()
{
#ifdef DEBUG_SOCKETMONITOR
	qDebug("Socket: %s", __PRETTY_FUNCTION__);
#endif
	return true;
}
