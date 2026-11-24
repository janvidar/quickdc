/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <samurai/debug/dbg.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/url.h>

#include "api/core.h"
#include "network/connection.h"
#include "network/connectionmanager.h"
#include "network/dc/dcconnection.h"

QuickDC::Connection::Connection(Core& core_, std::shared_ptr<Samurai::IO::Net::Socket> sock, const char* proto)
	: core(core_), socket(std::move(sock)), manager(core_.connections), protocol(proto)
{
	socket->setEventHandler(this);
}


QuickDC::Connection::Connection(Core& core_, Samurai::IO::Net::URL* url, const char* proto)
	: core(core_), manager(core_.connections), protocol(proto)
{
	socket = Samurai::IO::Net::Socket::create(core.loop(), this, url->getHost(), url->getPort());
}

QuickDC::Connection::~Connection()
{
	if (manager)
	{
		manager->detach(this);
	}
	socket.reset();
}


