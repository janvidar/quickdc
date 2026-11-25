/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"

#include <string.h>
#include "api/core.h"
#include "network/reachability.h"
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


QuickDC::DCServer::DCServer(Core& core_, const Endpoint& endpoint)
	: core(core_)
{
	listen(endpoint);
}

void QuickDC::DCServer::listen(const Endpoint& endpoint)
{
	dgram6.reset();
	socket6.reset();
	dgram.reset();
	socket.reset();

	at = endpoint;

	const std::string& bindaddr = at.bind;
	const std::string& bindaddr6 = at.bind6;

	QDBG("Starting up server at port %d, listening on %s", at.port, bindaddr.c_str());

	Samurai::IO::Net::InetAddress localAddr(bindaddr.c_str());

	/*
	 * Port 0 is one port picked for both: TCP is given one, and UDP is asked for
	 * the same number. One taken for UDP already is given back and another one
	 * picked, so a peer told the port finds both services on it.
	 */
	uint16_t port = at.port;
	for (int attempt = 0; attempt < QUICKDC_PICK_PORT_ATTEMPTS; attempt++)
	{
		dgram.reset();
		socket = Samurai::IO::Net::ServerSocket::create(core.loop(), this,
			Samurai::IO::Net::InetSocketAddress(localAddr, at.port));

		if (!socket->listen())
		{
			QDBG("Error: could not bind to port for TCP service. Disabling service");
			socket.reset();
		}

		port = (at.port || !socket) ? at.port : socket->getLocalPort();
		dgram = Samurai::IO::Net::DatagramSocket::create(core.loop(), this,
			Samurai::IO::Net::InetSocketAddress(localAddr, port));

		if (dgram->listen()) break;

		QDBG("Error: could not bind to port %d for UDP service", port);
		dgram.reset();
		if (at.port || !socket) break;
	}

	if (!at.port && socket && !dgram)
	{
		QDBG("No port was free for both TCP and UDP; disabling the peer port");
		socket.reset();
		port = 0;
	}

	/*
	 * The same port on the other family, in sockets of its own: a wildcard bound
	 * for IPv6 answers for IPv6 alone - see SocketBase::setV6Only() - so the pair
	 * above is untouched by this one whether or not it comes up.
	 *
	 * A host with no IPv6 fails to bind here and is left as it was. That is not an
	 * error to report: a machine that has no IPv6 is not misconfigured, and the
	 * client is simply reachable on one family. Nothing is advertised for a family
	 * that has no socket, so a peer is never sent to a port nothing is listening on.
	 */
	if (bindaddr6.empty()) return;

	Samurai::IO::Net::InetAddress localAddr6(bindaddr6.c_str(),
	                                         Samurai::IO::Net::InetAddress::Version::IPv6);
	Samurai::IO::Net::InetSocketAddress bindAddr6(localAddr6, port);

	socket6 = Samurai::IO::Net::ServerSocket::create(core.loop(), this, bindAddr6);
	dgram6  = Samurai::IO::Net::DatagramSocket::create(core.loop(), this, bindAddr6);

	if (!socket6 || !socket6->listen())
	{
		QDBG("No IPv6 peer port: could not listen on %s", bindaddr6.c_str());
		socket6.reset();
	}

	if (!dgram6 || !dgram6->listen())
	{
		QDBG("No IPv6 search port: could not listen on %s", bindaddr6.c_str());
		dgram6.reset();
	}
}

uint16_t QuickDC::DCServer::boundPort() const
{
	if (socket) return socket->getLocalPort();
	if (socket6) return socket6->getLocalPort();
	return 0;
}

uint16_t QuickDC::DCServer::boundUdpPort() const
{
	if (dgram) return dgram->getLocalPort();
	if (dgram6) return dgram6->getLocalPort();
	return 0;
}

QuickDC::DCServer::~DCServer()
{
	dgram6.reset();
	socket6.reset();
	dgram.reset();
	socket.reset();
}

void QuickDC::DCServer::EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char* msg)
{
	QERR("Socket accept error: %s", msg);
}

void QuickDC::DCServer::EventAcceptSocket(const Samurai::IO::Net::ServerSocket*, std::shared_ptr<Samurai::IO::Net::Socket> socket)
{
	/* Null for a peer that has already gone, which an accepted socket's may have
	   done before anything asks where it was. */
	const Samurai::IO::Net::InetAddress* from = socket->getAddress();

#ifdef DATADUMP
	QDBG("Socket accepted: %s", from ? from->toString().c_str() : "(unknown)");
#endif

	/*
	 * Somebody reached the peer port from outside, which is the one thing that
	 * proves it can be reached - a claim to be active rests on this and nothing
	 * else can establish it.
	 *
	 * Not the loopback: a connection from this machine proves the socket is open,
	 * which was never the question. Nor an address that cannot be read, since
	 * there is then no telling which of the two it was.
	 */
	if (from && !from->isLoopback())
	{
		/*
		 * Against the family it arrived on, which is now a distinction worth
		 * making: the two ports are two endpoints, and a peer that reached us
		 * over IPv6 has proven nothing whatever about the IPv4 one.
		 */
		const bool six =
			from->getType() == Samurai::IO::Net::InetAddress::Version::IPv6;

		if (Reachability* record = core.reachabilityFor(six))
			record->inboundConnection();
	}

	Connection* connection = new ProtocolProbe(core, socket);
	socket->setEventHandler(connection);
	socket->setNonBlocking(true);
}

void QuickDC::DCServer::EventGotDatagram(Samurai::IO::Net::DatagramSocket* sock, Samurai::IO::Net::DatagramPacket* packet)
{
	ProtocolProbe::probeDatagram(core, sock, packet);
}

void QuickDC::DCServer::EventDatagramError(const Samurai::IO::Net::DatagramSocket*, const char* msg)
{
	QERR("Datgram/UDP error: %s", msg);
}

void QuickDC::DCServer::send(Samurai::IO::Net::DatagramPacket* packet)
{
	Samurai::IO::Net::SocketAddress* to = packet ? packet->getAddress() : nullptr;
	if (!to) return;

	/*
	 * Out of the socket bound to the family being addressed. Each answers for one
	 * family and neither can reach the other's, so sending a datagram meant for an
	 * IPv6 peer down the IPv4 socket is not a slower route to it but no route at
	 * all - and the search it answers would go unanswered with nothing said.
	 */
	const bool six = to->getSockAddrFamily() == AF_INET6;

	// NOTE: Might not be initialized if socket was busy
	if (six)
	{
		if (dgram6) dgram6->send(packet);
		return;
	}

	if (dgram) dgram->send(packet);
}

