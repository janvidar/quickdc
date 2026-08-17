/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SERVER_API_H
#define HAVE_QUICKDC_SERVER_API_H

#include "quickdc.h"
#include <memory>
#include <samurai/io/net/socketevent.h>

namespace {
	namespace IO {
		namespace Net {
			class ServerSocket;
			class DatagramSocket;
			class DatagramPacket;
		}
	}

}

namespace QuickDC {

class DCServer : public Samurai::IO::Net::ServerSocketEventHandler, public Samurai::IO::Net::DatagramEventHandler {
	public:
		DCServer();
		virtual ~DCServer();
		
		void send(Samurai::IO::Net::DatagramPacket*);
		
	protected:
		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char* msg) override;
		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*, std::shared_ptr<Samurai::IO::Net::Socket> socket) override;
		void EventGotDatagram(Samurai::IO::Net::DatagramSocket*, Samurai::IO::Net::DatagramPacket*) override;
		void EventDatagramError(const Samurai::IO::Net::DatagramSocket*, const char* msg) override;

	protected:
		std::shared_ptr<Samurai::IO::Net::ServerSocket> socket;
		std::shared_ptr<Samurai::IO::Net::DatagramSocket> dgram;

		/**
		 * The same two on IPv6, null where the host has none to bind.
		 *
		 * Sockets of their own rather than one pair carrying both families,
		 * because what carries what is a decision the system makes differently
		 * from platform to platform - see SocketBase::setV6Only(). A pair each
		 * is the arrangement that reads the same everywhere, and it is also
		 * what lets one family be listening while the other is not.
		 */
		std::shared_ptr<Samurai::IO::Net::ServerSocket> socket6;
		std::shared_ptr<Samurai::IO::Net::DatagramSocket> dgram6;
};

}

#endif // HAVE_QUICKDC_SERVER_API_H
