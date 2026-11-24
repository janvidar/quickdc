/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <atomic>
#include "network/connectionmanager.h"
#include "network/connection.h"

#include <samurai/eventloop.h>

namespace {

/*
 * Handed out once per deferred deletion ever queued, process-wide rather than
 * per manager.
 *
 * The message handler these tickets travel through belongs to the loop and
 * outlives any one manager, so a message posted by a manager that has since gone
 * is still delivered - to every manager on that loop. Numbering from one in each of them would
 * have the survivor recognise the dead one's ticket as its own and free a
 * connection that is still in use. Same reasoning as the timer serial in
 * samurai's timer.cpp.
 */
uint64_t next_ticket()
{
	static std::atomic<uint64_t> counter{0};
	return ++counter;
}

}

QuickDC::ConnectionManager::ConnectionManager(Samurai::EventLoop& loop)
	: Samurai::MessageListener(loop)
	, loop(loop)
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

	/*
	 * And the ones waiting for a message that will not be delivered before this
	 * object goes away. Every one of them is still alive: a connection destroyed
	 * by whoever else held it reaches ~Connection, which detaches, which is what
	 * takes it out of here.
	 *
	 * Emptying the map is also what makes the queued messages harmless. They
	 * outlive this object - the handler they were posted to is the loop's - and
	 * each one resolves to nothing when it finally arrives.
	 */
	while (pending.size())
	{
		QuickDC::Connection* conn = (*pending.begin()).second;
		pending.erase(pending.begin());
		conn->setManager(0);
#ifdef DATADUMP
		QDBG("Delete connection: %p (2)", conn);
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

	/*
	 * And out of the deletions still to be made, which is the dangerous half. A
	 * connection reaches here from its own destructor, so a ticket left naming it
	 * is a delete of freed memory whenever the message it belongs to arrives - or
	 * at the latest from this manager's own destructor.
	 */
	forget(conn);

	conn->setManager(0);
}

void QuickDC::ConnectionManager::forget(Connection* conn) {
	std::map<uint64_t, Connection*>::iterator ticket = pending.begin();
	while (ticket != pending.end())
	{
		if ((*ticket).second == conn) ticket = pending.erase(ticket);
		else ticket++;
	}
}

bool QuickDC::ConnectionManager::isPending(const Connection* conn) const {
	for (const auto& [ticket, queued] : pending)
		if (queued == conn) return true;
	return false;
}

void QuickDC::ConnectionManager::remove(Connection* conn) {
	if (!conn) return;

	/* Not ours, or already on its way out; either way, not to be queued twice. */
	if (conn->getManager() != this) return;
	if (isPending(conn)) return;

	for (std::vector<Connection*>::iterator it = connections.begin(); it != connections.end(); it++)
	{
		if (*it == conn) {
			connections.erase(it);
			break;
		}
	}

	/*
	 * Deliberately still pointing at this manager, where detach() would have
	 * cleared it. The connection is queued for deletion rather than disowned, and
	 * the whole of what keeps that safe is that ~Connection can still reach here
	 * to cancel it - a session that its owner deletes before the message arrives
	 * is the ordinary case, not a misuse.
	 */
	const uint64_t ticket = next_ticket();
	pending.insert(std::pair<uint64_t, Connection*>(ticket, conn));

	loop.messages().postMessage(QuickDC::MsgConnectionDropped, 0, static_cast<size_t>(ticket), 0);
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
		/*
		 * Not ours resolves to nothing, which is what has to happen: every
		 * manager sees every message, so one posted by another manager - or by
		 * one that has since been destroyed - arrives here too.
		 */
		const std::map<uint64_t, Connection*>::iterator it =
			pending.find(static_cast<uint64_t>(msg->getArg1()));
		if (it == pending.end())
			return false;

		QDBG("EventMessage: MsgConnectionDropped");
		Connection* conn = (*it).second;
		pending.erase(it);
		conn->setManager(0);
		delete conn;
		return true;
	}
	return false;
}

