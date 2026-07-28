/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <samurai/io/net/socket.h>
#include <vector>

#include "network/socketreaper.h"

static std::vector<std::shared_ptr<Samurai::IO::Net::Socket> >& retired()
{
	static std::vector<std::shared_ptr<Samurai::IO::Net::Socket> > sockets;
	return sockets;
}

void QuickDC::retireSocket(std::shared_ptr<Samurai::IO::Net::Socket> socket)
{
	if (!socket) return;
	retired().push_back(std::move(socket));
}

void QuickDC::reapSockets()
{
	/*
	 * Destroying a socket can retire another - a hub user dropped along with
	 * the connection that carried it - so swap the list out rather than
	 * iterating one that may grow underneath.
	 */
	while (!retired().empty())
	{
		std::vector<std::shared_ptr<Samurai::IO::Net::Socket> > batch;
		batch.swap(retired());
	}
}
