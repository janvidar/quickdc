/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include <string.h>
#include "api/core.h"
#include "api/server.h"
#include "config/preferences.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketbase.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socketmonitor.h>
#include "network/probe.h"
#include "network/dc/dcconnection.h"
#include "network/connectionmanager.h"

QuickDC::Core* quickdc_core;

QuickDC::DCServer::DCServer()
{
	Preferences* config = Core::getInstance()->config;
	config->setGroup("Network");

	uint16_t port = config->getNumber("Port", 1411);
	std::string bindaddr = config->getString("Bind", "0.0.0.0");

	QDBG("Starting up server at port %d, listening on %s", port, bindaddr.c_str());

	Samurai::IO::Net::InetAddress localAddr(bindaddr.c_str());
	Samurai::IO::Net::InetSocketAddress bindAddr(localAddr, port);

	socket = Samurai::IO::Net::ServerSocket::create(this, bindAddr);
	dgram  = Samurai::IO::Net::DatagramSocket::create(this, bindAddr);
	
	if (!socket->listen())
	{
		QDBG("Error: could not bind to port for TCP service. Disabling service");
		socket.reset();
	}
	       
	if (!dgram->listen())
	{
		QDBG("Error: could not bind to port for UDP service. Disabling service");
		dgram.reset();
	}
}

QuickDC::DCServer::~DCServer()
{
	dgram.reset();
	socket.reset();
}

void QuickDC::DCServer::EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char* msg)
{
	QERR("Socket accept error: %s", msg);
}

void QuickDC::DCServer::EventAcceptSocket(const Samurai::IO::Net::ServerSocket*, std::shared_ptr<Samurai::IO::Net::Socket> socket)
{
#ifdef DATADUMP
	/* Null for a peer that has already gone, which an accepted socket's may have
	   done before anything asks where it was. */
	QDBG("Socket accepted: %s", socket->getAddress()
		? socket->getAddress()->toString().c_str() : "(unknown)");
#endif
	Connection* connection = new ProtocolProbe(socket);
	socket->setEventHandler(connection);
	socket->setNonBlocking(true);
}

void QuickDC::DCServer::EventGotDatagram(Samurai::IO::Net::DatagramSocket* sock, Samurai::IO::Net::DatagramPacket* packet)
{
	ProtocolProbe::probeDatagram(sock, packet);
}

void QuickDC::DCServer::EventDatagramError(const Samurai::IO::Net::DatagramSocket*, const char* msg)
{
	QERR("Datgram/UDP error: %s", msg);
}

void QuickDC::DCServer::send(Samurai::IO::Net::DatagramPacket* packet)
{
	// NOTE: Might not be initialized if socket was busy
	if (dgram)
	{
		dgram->send(packet);
	}
}

