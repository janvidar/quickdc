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
#include "network/adc/adcudp.h"
#include "network/adc/server/adchub.h"
#include "network/http/httpconnection.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/datagram.h>
#include <samurai/io/buffer.h>
#include "config/preferences.h"

#include <string.h>

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

/* Unsigned: char is signed here, so a byte with the top bit set sign extends
   into the whole word, and shifting a negative int left is undefined besides. */
static uint32_t fourcc_of(const char* buffer)
{
	return (((uint32_t) (unsigned char) buffer[0] << 24)
	      | ((uint32_t) (unsigned char) buffer[1] << 16)
	      | ((uint32_t) (unsigned char) buffer[2] << 8)
	      |  (uint32_t) (unsigned char) buffer[3]);
}

static bool probe_tcp_tls(const char* buffer, size_t length) {
	if (length < 16) return false;

	uint8_t protocol_version_maj;
	uint8_t protocol_version_min;
	uint8_t msg_type;

	bool is_ssl = false;

	if (buffer[0] == 22) {
		/*
		 * SSLv3+/TLS handshake check: record type 22, the record version, and
		 * at offset 5 the handshake message type - 1 for a ClientHello, whose
		 * own version follows at offset 9. Everything from SSL 3.0 onwards has
		 * a version major of 3.
		 *
		 * The two versions are deliberately not required to agree with each
		 * other. A TLS 1.3 client sends a 1.0 record version carrying a 1.2
		 * client version for middlebox compatibility, and OpenSSL does the same
		 * for 1.2, so insisting on a match rejected every current client.
		 */
		protocol_version_maj = buffer[1];
		protocol_version_min = buffer[2];
		msg_type = buffer[5];
		if (msg_type == 1 &&
		    protocol_version_maj == 3 &&
		    buffer[9] == 3)
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
		/* The minor version decides nothing above; it is only reported. */
		(void) protocol_version_min;
		return true;
	}

	return false;
}

/*
 * Probe for ADC connection
 */
static bool probe_tcp_adc(const char* buffer, size_t length) {
	if (length < 4) return false;
	uint32_t fourcc = fourcc_of(buffer);
	if (FOURCC('C', 'S', 'U', 'P') == fourcc)
		return true;
	return false;
}

/*
 * Probe for ADC hub connection
 */
static bool probe_tcp_adchub(const char* buffer, size_t length) {
	if (length < 4) return false;
	uint32_t fourcc = fourcc_of(buffer);
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
		case FOURCC('U','P','S','R'):
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
	uint32_t fourcc = fourcc_of(buffer);
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


QuickDC::Protocol QuickDC::identify(const char* buffer, size_t length)
{
	if (!buffer) return Protocol::Unknown;

	if (probe_tcp_adchub(buffer, length)) return Protocol::ADCHub;
	if (probe_tcp_dc(buffer, length))     return Protocol::DC;
	if (probe_tcp_adc(buffer, length))    return Protocol::ADC;
	if (probe_tcp_http(buffer, length))   return Protocol::HTTP;
	if (probe_tcp_tls(buffer, length))    return Protocol::TLS;

	return Protocol::Unknown;
}

QuickDC::Protocol QuickDC::identifyDatagram(const char* buffer, size_t length)
{
	if (!buffer || length < 4) return Protocol::Unknown;

	const uint32_t fourcc = fourcc_of(buffer);

	if (probe_udp_adc(fourcc))    return Protocol::ADC;
	if (probe_udp_adchub(fourcc)) return Protocol::ADCHub;
	if (probe_udp_dc(fourcc))     return Protocol::DC;

	return Protocol::Unknown;
}


QuickDC::ProtocolProbe::ProtocolProbe(std::shared_ptr<Samurai::IO::Net::Socket> sock) : QuickDC::Connection(std::move(sock), "Probing"), secure(false) {
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
		socket.reset();
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
	const char head[4] = { buffer->at(0), buffer->at(1), buffer->at(2), buffer->at(3) };

	switch (QuickDC::identifyDatagram(head, sizeof(head))) {
		case Protocol::ADC:
			ADC::DatagramHandler::onDatagram(socket, packet);
			break;

		case Protocol::ADCHub:
		{
			ADC::Hub* hub = QuickDC::Core::getInstance()->localHub;
			if (hub) {
				(void) socket;
				// hub->onDatagram(socket, packet);
			}
			break;
		}

		case Protocol::DC:
			/* handle NMDC packet */
			break;

		default:
			/* ignore packet */
#ifdef DATADUMP
			QDBG("UDP junk");
#endif // DATADUMP
			break;
	}
}

void QuickDC::ProtocolProbe::EventTLSConnected(const Samurai::IO::Net::Socket*) {
	QDBG("TLS handshake complete; fingerprinting the decrypted stream");
	secure = true;
	probe();
}

void QuickDC::ProtocolProbe::probe() {
	char buf[16] = {0,};

	/*
	 * Not the result returning peek(): that one reads the descriptor directly
	 * and never consults the TLS layer, so on an ADCS connection it hands back
	 * the encrypted record rather than the handshake we are here to fingerprint,
	 * and every probe below fails on it. This overload does go through TLS, at
	 * the cost of answering 0 for would-block, end of stream and error alike.
	 *
	 * Nothing to look at yet is the normal state right after a TLS handshake
	 * completes - the peer's first application data has not arrived - so wait
	 * for more instead of dropping the connection. Nothing is leaked by
	 * waiting: the 30 second timer closes a connection that stays silent.
	 */
	ssize_t len = socket->peek(buf, 16);
	if (len <= 0) return;

	switch (QuickDC::identify(buf, (size_t) len)) {
		case Protocol::ADCHub:
		{
			ADC::Hub* hub = QuickDC::Core::getInstance()->localHub;
			if (hub) {
#ifdef DATADUMP
				QDBG("ADC hub connection");
#endif
				hub->onConnected(socket);
				socket->setEventHandler(hub);
				socket.reset();
			}
			break;
		}

		case Protocol::DC:
		{
#ifdef DATADUMP
			QDBG("DC connection");
#endif // DATADUMP
			DC::ClientSession* connection = new DC::ClientSession(socket);
			socket->setEventHandler(connection);
			socket.reset();
			break;
		}

		case Protocol::ADC:
		{
#ifdef DATADUMP
			QDBG("ADC connection");
#endif // DATADUMP
			ADC::ClientSession* connection = new ADC::ClientSession(socket, 0, 0, 0, secure);
			socket->setEventHandler(connection);
			socket.reset();
			break;
		}

		case Protocol::HTTP:
		{
#ifdef DATADUMP
			QDBG("HTTP connection");
#endif // DATADUMP
			QuickDC::Preferences* config = QuickDC::Core::getInstance()->config;
			config->setGroup("HTTP server");
			if (config->getBool("Enabled", false)) {
				Http::Connection* connection = new Http::Connection(socket, false);
				socket->setEventHandler(connection);
				socket.reset();
			} else {
				QERR("HTTP server is disabled -- closing connection");
			}
			break;
		}

		case Protocol::TLS:
		{
			/* Nothing can be fingerprinted through TLS, so finish the handshake
			   and probe again once the decrypted stream is readable. Already
			   inside one, this is not a handshake but a stream we cannot read. */
			if (secure) break;

			QDBG("TLS connection; deferring the probe until the handshake is done");
			if (!socket->TLSInitialize(true))
			{
				QERR("Unable to accept a TLS connection");
				close();
				return;
			}

			/*
			 * One pass rarely completes a handshake. The socket resumes it as
			 * the connection becomes readable or writable again and reports the
			 * result through EventTLSConnected(), so it has to be left alone
			 * until then - closing it here is what made an inbound ADCS
			 * connection look like a reset to the peer.
			 */
			socket->TLSsendHandshake();
			return;
		}

		case Protocol::Unknown:
			QDBG("Unknown connection handshake -- closing connection");
			break;
	}

	close();
}

void QuickDC::ProtocolProbe::EventDisconnected(const Samurai::IO::Net::Socket*) {
	QDBG("Disconnected");
	close();
}

void QuickDC::ProtocolProbe::EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError error, const char* msg)
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


