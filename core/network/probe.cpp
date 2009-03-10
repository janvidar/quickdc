/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/core.h"
#include "network/probe.h"
#include "network/connectionmanager.h"
#include "network/dc/dcconnection.h"
#include "network/dc/clientsession.h"
#include "network/adc/client/adcclientsession.h"
#include "network/adc/server/adchub.h"
#include "network/http/httpconnection.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/buffer.h>
#include "config/preferences.h"

#include <string.h>

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

#ifdef SSL_SUPPORT
static bool probe_tcp_tls(const char* buffer, size_t length) {
	if (length < 16) return false;

	uint8_t protocol_version_maj;
	uint8_t protocol_version_min;
	uint8_t msg_type;

	bool is_ssl = false;

	if (buffer[0] == 22) {
		/* SSLv3+/TLS handshake check */
		protocol_version_maj = buffer[1];
		protocol_version_min = buffer[2];
		msg_type = buffer[5];
		if (msg_type == 1 &&
		    protocol_version_maj == 3 && 
		    protocol_version_maj == buffer[ 9] &&
		    protocol_version_min == buffer[10])
		{
			is_ssl = true;	
		}
	} else if (buffer[0] & 0x80) {
		/* SSLv2 handshake check */
		msg_type = buffer[2];
		protocol_version_maj = buffer[3];
		protocol_version_min = buffer[4];
		if (msg_type == 1 && (protocol_version_maj == 3)) {
			is_ssl = true;
		}
	}

	if (is_ssl) {
		QDBG("Probed SSL/TLS version %d.%d", protocol_version_maj, protocol_version_min);
		return true;
	}

	return false;
}
#endif // SSL_SUPPORT

/*
 * Probe for ADC connection
 */
static bool probe_tcp_adc(const char* buffer, size_t length) {
	if (length < 4) return false;
	uint32_t fourcc = ((buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | (buffer[3]));
	if (FOURCC('C', 'S', 'U', 'P') == fourcc)
		return true;
	return false;
}

/*
 * Probe for ADC hub connection
 */
static bool probe_tcp_adchub(const char* buffer, size_t length) {
	if (length < 4) return false;
	uint32_t fourcc = ((buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | (buffer[3]));
	if (FOURCC('H', 'S', 'U', 'P') == fourcc) return true;
	return false;
}

/*
 * Probe for ADC datagrams targetted for the client.
 */
static bool probe_udp_adc(uint32_t fourcc) {
	switch (fourcc) {
		case FOURCC('U','S','C','H'):
		case FOURCC('U','R','E','S'):
		case FOURCC('U','S','T','A'):
#ifdef NAT_TRAVERSAL_SUPPORT
		case FOURCC('U','T','U','N'):
		case FOURCC('U','E','C','O'):
#endif
			return true;
		default:
			return false;
	}
}


/*
 * Probe for ADC datagrams targetted for the hub.
 */
static bool probe_udp_adchub(uint32_t fourcc) {
#ifdef NAT_TRAVERSAL_SUPPORT
	if (FOURCC('H','T','U','N') == fourcc) return true;
	if (FOURCC('H','E','C','O') == fourcc) return true;
#else
	(void) fourcc;
#endif
	return false;
}


/*
 * Probe for NMDC datagram
 */
static bool probe_udp_dc(uint32_t fourcc) {
	switch (fourcc) {
		case FOURCC('$','S','R',' '):
		case FOURCC('$','P','i','n'):
			return true;
		default:
			return false;
	}
}


/*
 * Probe for NMDC connection
 */
static bool probe_tcp_dc(const char* buffer, size_t length) {
	if (length < 8) return false;
	return (strncmp(buffer, "$MyNick ", 8) == 0);
}

/*
 * Probe for HTTP connection
 */
static bool probe_tcp_http(const char* buffer, size_t length) {
	if (length < 4) return false;
	uint32_t fourcc = ((buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | (buffer[3]));
	switch (fourcc) {
		case FOURCC('G', 'E', 'T', ' '): // GET
		case FOURCC('H', 'E', 'A', 'D'): // HEAD
		case FOURCC('P', 'O', 'S', 'T'): // POST
		case FOURCC('P', 'U', 'T', ' '): // PUT
		case FOURCC('T', 'R', 'A', 'C'): // TRAC(E)
		case FOURCC('D', 'E', 'L', 'E'): // DELE(TE)
		case FOURCC('O', 'P', 'T', 'I'): // OPTI(ONS)
			return true;
		default:
			return false;
	}
}


QuickDC::ProtocolProbe::ProtocolProbe(Samurai::IO::Net::Socket* sock) : QuickDC::Connection(sock, "Probing") {
	timer = new Samurai::Timer(this, 30, true); /* if nothing happens in 30 seconds, close the connection */
	manager->add(this);
}

QuickDC::ProtocolProbe::~ProtocolProbe() {
	close();
}

void QuickDC::ProtocolProbe::close() {
	delete timer;
	timer = 0;

	if (socket && socket->getEventHandler() == this) {
		delete socket;
		socket = 0;
	}
	if (manager) manager->remove(this);
}


void QuickDC::ProtocolProbe::EventDataAvailable(const Samurai::IO::Net::Socket*) {
	probe();
}


void QuickDC::ProtocolProbe::EventTimeout(Samurai::Timer*) {
	close();
}

void QuickDC::ProtocolProbe::probeDatagram(Samurai::IO::Net::DatagramSocket* socket, Samurai::IO::Net::DatagramPacket* packet) {
	if (packet->size() < 4) return;
	Samurai::IO::Buffer* buffer = packet->getBuffer();
	uint32_t fourcc = ((buffer->at(0) << 24) | (buffer->at(1) << 16) | (buffer->at(2) << 8) | (buffer->at(3)));
	
	if (probe_udp_adc(fourcc)) {
		/* handle ADC packet */
		
	} else if (probe_udp_adchub(fourcc)) {
		/* handle ADC hub packet */
		
		ADC::Hub* hub = QuickDC::Core::getInstance()->localHub;
		if (hub) {
			(void) socket;
			// hub->onDatagram(socket, packet);
		}
	

	} else if (probe_udp_dc(fourcc)) {
		/* handle NMDC packet */
 	} else {
		/* ignore packet */
#ifdef DATADUMP
		QDBG("UDP junk");
#endif // DATATUMP
	}
}

void QuickDC::ProtocolProbe::probe() {
	static char buf[16] = {0,};
	int len = socket->peek(buf, 16);

	if (len > 0) {
		if (probe_tcp_adchub(buf, (size_t) len)) {
			ADC::Hub* hub = QuickDC::Core::getInstance()->localHub;
			if (hub) {
#ifdef DATADUMP
				QDBG("ADC hub connection");
#endif
				hub->onConnected(socket);
				socket->setEventHandler(hub);
				socket = 0;
			}
	
		} else if (probe_tcp_dc(buf, (size_t) len)) {
#ifdef DATADUMP
			QDBG("DC connection");
#endif // DATADUMP
			DC::ClientSession* connection = new DC::ClientSession(socket);
			socket->setEventHandler(connection);
			socket = 0;

		} else if (probe_tcp_adc(buf, (size_t) len)) {
#ifdef DATADUMP
			QDBG("ADC connection");
#endif // DATADUMP
			ADC::ClientSession* connection = new ADC::ClientSession(socket);
			socket->setEventHandler(connection);
			socket = 0;

		} else if (probe_tcp_http(buf, (size_t) len)) {
#ifdef DATADUMP
			QDBG("HTTP connection");
#endif // DATADUMP
			QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
			config->setGroup("HTTP server");
			bool enabled = config->getBool("Enabled", false);
			if (enabled) {
				Http::Connection* connection = new Http::Connection(socket, false);
				socket->setEventHandler(connection);
				socket = 0;
			} else {
				QERR("HTTP server is disabled -- closing connection");
			}

#ifdef SSL_SUPPORT
		} else if (probe_tcp_tls(buf, (size_t) len)) {
			QDBG("Fixme: accept SSL connection");
			
#if 0
			ADC::Hub* hub = QuickDC::Core::getInstance()->localHub;
			if (hub) {
				socket->TLSInitialize(true);
				socket->TLSsendHandshake();

				hub->onConnected(socket);
				socket->setEventHandler(hub);
				socket = 0;
			}
#endif // 0
			
#endif // SSL_SUPPORT
		} else {
			QDBG("Unknown connection handshake -- closing connection");
		}
	}
	close();
}

void QuickDC::ProtocolProbe::EventDisconnected(const Samurai::IO::Net::Socket*) {
	QDBG("Disconnected")
	close();
}

void QuickDC::ProtocolProbe::EventError(const Samurai::IO::Net::Socket*, enum Samurai::IO::Net::SocketError error, const char* msg)
{
	(void) error;
	if (msg) QERR("Error caught: '%s'", msg);
	close();
}


void QuickDC::ProtocolProbe::EventHostFound(const Samurai::IO::Net::Socket*) { }
void QuickDC::ProtocolProbe::EventHostLookup(const Samurai::IO::Net::Socket*) { }
void QuickDC::ProtocolProbe::EventConnecting(const Samurai::IO::Net::Socket*) { }
void QuickDC::ProtocolProbe::EventConnected(const Samurai::IO::Net::Socket*) { }
void QuickDC::ProtocolProbe::EventTimeout(const Samurai::IO::Net::Socket*) { }
void QuickDC::ProtocolProbe::EventCanWrite(const Samurai::IO::Net::Socket*) { }


