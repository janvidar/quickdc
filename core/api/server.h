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
};

}

#endif // HAVE_QUICKDC_SERVER_API_H
