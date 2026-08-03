/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONNECTION_MANAGER_H
#define HAVE_QUICKDC_CONNECTION_MANAGER_H

#include "quickdc.h"
#include <samurai/io/net/socket.h>
#include <vector>

namespace QuickDC {
class Connection;

/**
 * This class manages any connection, both hubs and clients
 * for all protocols implemented.
 */
class ConnectionManager
	: public Samurai::MessageListener
{
	public:
		ConnectionManager();
		virtual ~ConnectionManager();
	
		virtual void add(Connection* conn);

		/**
		 * Drop a connection and have it deleted. Safe for a connection that was
		 * never added, and safe to call more than once. Deletion is deferred,
		 * since this is normally called from the connection's own event handler.
		 */
		virtual void remove(Connection* conn);

		/**
		 * Forget a connection without deleting it, for a connection that is
		 * already being destroyed.
		 */
		virtual void detach(Connection* conn);

		virtual void process();

		/**
		 * Returns the number of active connections.
		 */
		size_t size();

		/* Built-in iterators */
		Connection* first();
		Connection* next();

	protected:
		bool EventMessage(const Samurai::Message*) override;

	protected:
		std::vector<Connection*> connections;
		std::vector<Connection*>::iterator connectionIterator;
};

}

#endif // HAVE_QUICKDC_CONNECTION_MANAGER_H
