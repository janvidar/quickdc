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

QuickDC::Connection::Connection(std::shared_ptr<Samurai::IO::Net::Socket> sock, const char* proto)
	: socket(std::move(sock)), protocol(proto)
{
	socket->setEventHandler(this);
	manager = QuickDC::Core::getInstance()->connections;
}


QuickDC::Connection::Connection(Samurai::IO::Net::URL* url, const char* proto) : protocol(proto)
{
	socket = Samurai::IO::Net::Socket::create(this, url->getHost(), url->getPort());
	manager = QuickDC::Core::getInstance()->connections;
}

QuickDC::Connection::~Connection()
{
	if (manager)
	{
		manager->detach(this);
	}
	socket.reset();
}


