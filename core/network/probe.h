/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_PROTOCOL_PROBE_H
#define HAVE_QUICKDC_PROTOCOL_PROBE_H

#include "quickdc.h"
#include "network/connection.h"
#include <samurai/timer.h>

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
			class DatagramSocket;
			class DatagramPacket;
		}
	}
}


namespace QuickDC {

class ConnectionManager;

/**
 * What the first bytes of a connection look like, before anything at all is
 * known about who sent them.
 */
enum class Protocol { Unknown, TLS, ADC, ADCHub, DC, HTTP };

/**
 * Fingerprint the first 'length' bytes of a stream.
 *
 * @return Unknown for anything we do not speak, which includes there being too
 *         few bytes to tell one from another yet.
 */
Protocol identify(const char* buffer, size_t length);

/**
 * The same for a datagram, which is told apart by its four character code
 * alone.
 */
Protocol identifyDatagram(const char* buffer, size_t length);

/**
 * The protocol probe is used to identify the type of request given 
 * to it and assign an apropriate connection handler.
 *
 * Typically when a connection is made to QuickDC the connection
 * is assigned to a protocol probe. The probe will fingerprint
 * which protocol is used, and assign the apropriate protocol handler
 * to deal with it.
 *
 * Currently the following protocols can be identified:
 * - DC
 * - ADC
 * - HTTP
 */
class ProtocolProbe : public Connection, public Samurai::TimerListener {
	public:
		ProtocolProbe(Core& core, std::shared_ptr<Samurai::IO::Net::Socket> sock);
		virtual ~ProtocolProbe();

		ProtocolProbe(const ProtocolProbe&) = delete;
		ProtocolProbe& operator=(const ProtocolProbe&) = delete;

		static void probeDatagram(Core& core, Samurai::IO::Net::DatagramSocket* socket, Samurai::IO::Net::DatagramPacket* packet);
		void probe();
		void close();

	protected:
		void EventTimeout(Samurai::Timer* timer) override;
		void EventHostLookup(const Samurai::IO::Net::Socket*) override;
		void EventHostFound(const Samurai::IO::Net::Socket*) override;
		void EventConnecting(const Samurai::IO::Net::Socket*) override;
		void EventConnected(const Samurai::IO::Net::Socket*) override;
		void EventTimeout(const Samurai::IO::Net::Socket*) override;
		void EventDisconnected(const Samurai::IO::Net::Socket*) override;
		void EventDataAvailable(const Samurai::IO::Net::Socket*) override;
		void EventCanWrite(const Samurai::IO::Net::Socket*) override;
		void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char*) override;
		void EventTLSConnected(const Samurai::IO::Net::Socket*) override;

	protected:
		Samurai::Timer* timer;

		/**
		 * Set once a TLS handshake has completed, so the second pass of probe()
		 * fingerprints the decrypted stream rather than the ClientHello.
		 */
		bool secure;
};

}

#endif // HAVE_QUICKDC_PROTOCOL_PROBE_H
