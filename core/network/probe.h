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
		ProtocolProbe(Samurai::IO::Net::Socket* sock);
		virtual ~ProtocolProbe();

		static void probeDatagram(Samurai::IO::Net::DatagramSocket* socket, Samurai::IO::Net::DatagramPacket* packet);
		void probe();
		void close();

	protected:
		void EventTimeout(Samurai::Timer* timer);
		void EventHostLookup(const Samurai::IO::Net::Socket*);
		void EventHostFound(const Samurai::IO::Net::Socket*);
		void EventConnecting(const Samurai::IO::Net::Socket*);
		void EventConnected(const Samurai::IO::Net::Socket*);
		void EventTimeout(const Samurai::IO::Net::Socket*);
		void EventDisconnected(const Samurai::IO::Net::Socket*);
		void EventDataAvailable(const Samurai::IO::Net::Socket*);
		void EventCanWrite(const Samurai::IO::Net::Socket*);
		void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char*);

	protected:
		Samurai::Timer* timer;
};

}

#endif // HAVE_QUICKDC_PROTOCOL_PROBE_H
