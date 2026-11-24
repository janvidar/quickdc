/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONNECTION_MANAGER_H
#define HAVE_QUICKDC_CONNECTION_MANAGER_H

#include "quickdc.h"
#include <samurai/io/net/socket.h>
#include <cstdint>
#include <map>
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
		/** @param loop where a dropped connection's deletion is posted to. */
		explicit ConnectionManager(Samurai::EventLoop& loop);
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
		 * already being destroyed. This also cancels a deletion remove() has
		 * queued for it, so a connection its owner frees first is not freed twice.
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

		/** Cancel every queued deletion naming this connection. */
		void forget(Connection* conn);

		/** Whether a deletion is already queued for this connection. */
		bool isPending(const Connection* conn) const;

	protected:
		std::vector<Connection*> connections;
		std::vector<Connection*>::iterator connectionIterator;

		/*
		 * Connections waiting to be deleted, named by a ticket rather than carried
		 * as a pointer in the message itself.
		 *
		 * The loop's message handler outlives any one manager, so a message posted
		 * from here can be delivered after the connection it named has gone
		 * another way: its owner deleted it first, or this manager was destroyed
		 * with it still queued. A ticket that no longer
		 * resolves is an instruction to do nothing, and this map is what makes it
		 * stop resolving; a pointer in the message would have been a delete of
		 * freed memory either way. Same reasoning, and the same shape, as the
		 * pending map in ADC::LocalHub::Hub.
		 */
		std::map<uint64_t, Connection*> pending;

		Samurai::EventLoop& loop;
};

}

#endif // HAVE_QUICKDC_CONNECTION_MANAGER_H
