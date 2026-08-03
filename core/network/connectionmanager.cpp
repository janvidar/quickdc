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

/*
 * NOTE: these two loops used to be written as
 *
 *     for (it = connections.begin(); *it; it++)
 *
 * which dereferences the iterator without ever comparing it against end().
 * A connection that is not in the vector - a peer session that is dropped
 * before it finished connecting, say - therefore ran off the end reading
 * whatever followed, and erase() then memmoved an out of range interval,
 * which is a bus error rather than anything recoverable.
 */

void QuickDC::ConnectionManager::add(Connection* conn)
{
	// Check if the connection is already added...
	for (std::vector<Connection*>::iterator it = connections.begin(); it != connections.end(); it++)
		if (*it == conn)
			return;

	connections.push_back(conn);
}

void QuickDC::ConnectionManager::detach(Connection* conn) {
	if (!conn) return;

	for (std::vector<Connection*>::iterator it = connections.begin(); it != connections.end(); it++)
	{
		if (*it == conn) {
			connections.erase(it);
			break;
		}
	}

	conn->setManager(0);
}

void QuickDC::ConnectionManager::remove(Connection* conn) {
	if (!conn) return;

	/* Already handed over; do not queue a second deletion. */
	if (conn->getManager() != this) return;

	detach(conn);

	Samurai::postMessage(QuickDC::MsgConnectionDropped, conn, 0, 0);
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
	if (msg->getID() == QuickDC::MsgConnectionDropped)
	{
		QDBG("EventMessage: MsgConnectionDropped");
		Connection* conn = static_cast<Connection*>(msg->getData());
		delete conn;
		return true;
	}
	return false;
}

