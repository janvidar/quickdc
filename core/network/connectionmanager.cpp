/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/connectionmanager.h"
#include "network/connection.h"

QuickDC::ConnectionManager::ConnectionManager()
{
}

QuickDC::ConnectionManager::~ConnectionManager()
{
	// Shutdown all connections
	while (connections.size())
	{
		QuickDC::Connection* conn = connections.back();
		connections.pop_back();
		conn->setManager(0);
#ifdef DATADUMP
		QDBG("Delete connection: %p (1)", conn);
#endif
		delete conn;
	}
}

void QuickDC::ConnectionManager::add(Connection* conn)
{
	// Check if the connection is already added...
	if (connections.size())
		for (std::vector<Connection*>::iterator it = connections.begin(); *it; it++)
			if (*it == conn)
				return;
	
	connections.push_back(conn);
}

void QuickDC::ConnectionManager::remove(Connection* conn) {
	if (connections.size())
	for (std::vector<Connection*>::iterator it = connections.begin(); *it; it++)
	{
		if (*it == conn) {
			connections.erase(it);
			conn->setManager(0);
			postMessage(Samurai::MsgConnectionDropped, conn, 0, 0);
			return;
		}
	}
}

void QuickDC::ConnectionManager::process() { }

size_t QuickDC::ConnectionManager::size() {
	return connections.size();
}

QuickDC::Connection* QuickDC::ConnectionManager::first() {
	connectionIterator = connections.begin();
	if (connectionIterator != connections.end())
		return *connectionIterator;
	return 0;
}

QuickDC::Connection* QuickDC::ConnectionManager::next() {
	connectionIterator++;
	if (connectionIterator != connections.end())
		return *connectionIterator;
	return 0;
}

bool QuickDC::ConnectionManager::EventMessage(const Samurai::Message* msg) {
	if (msg->id == Samurai::MsgConnectionDropped)
	{
		QDBG("EventMessage: MsgConnectionDropped");
		Connection* conn = (Connection*) msg->data;
		delete conn;
		return true;
	}
	return false;
}

