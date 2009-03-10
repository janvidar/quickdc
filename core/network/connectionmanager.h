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
		virtual void remove(Connection* conn);
		virtual void process();

		/**
		 * Returns the number of active connections.
		 */
		size_t size();

		/* Built-in iterators */
		Connection* first();
		Connection* next();

	protected:
		bool EventMessage(const Samurai::Message*);

	protected:
		std::vector<Connection*> connections;
		std::vector<Connection*>::iterator connectionIterator;
};

}

#endif // HAVE_QUICKDC_CONNECTION_MANAGER_H
