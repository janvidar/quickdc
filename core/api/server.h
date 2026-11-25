/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SERVER_API_H
#define HAVE_QUICKDC_SERVER_API_H

#include <string>
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

/** How many ports are picked for TCP before giving up on finding one UDP has free too. */
#define QUICKDC_PICK_PORT_ATTEMPTS 16

namespace QuickDC {

class Core;

class DCServer : public Samurai::IO::Net::ServerSocketEventHandler, public Samurai::IO::Net::DatagramEventHandler {
	public:
		/** Where the peer port is opened: the port, and each family's bind address. */
		struct Endpoint {
			uint16_t port = 1411;
			std::string bind = "0.0.0.0";
			/** Empty for no IPv6 peer port. */
			std::string bind6 = "::";

			bool operator==(const Endpoint&) const = default;
		};

		DCServer(Core& core, const Endpoint& at);

		/**
		 * Close the peer port and open it again at 'at'. Connections already
		 * accepted are not the server's and carry on. Not from inside one of
		 * the server's own socket callbacks.
		 */
		void listen(const Endpoint& at);

		const Endpoint& endpoint() const { return at; }
		virtual ~DCServer();
		
		void send(Samurai::IO::Net::DatagramPacket*);

		/**
		 * Whether a peer could reach this client on 'ipv6' at all.
		 *
		 * False where that family's socket never came up, which for IPv6 is every
		 * host that has none. Worth asking before advertising an endpoint or
		 * measuring one: both would otherwise be about a port nothing is
		 * listening on, and a measurement of it would come back blocked and be
		 * perfectly correct.
		 */
		bool listening(bool ipv6) const
		{
			return ipv6 ? socket6 != nullptr : socket != nullptr;
		}

		bool listeningUdp(bool ipv6) const
		{
			return ipv6 ? dgram6 != nullptr : dgram != nullptr;
		}

		/** The port peers are taken on, as bound - so the one picked where 0 was asked for - or 0. */
		uint16_t boundPort() const;

		/** The same for search results and other datagrams, which is the same port wherever both came up. */
		uint16_t boundUdpPort() const;
		
	protected:
		void EventAcceptError(const Samurai::IO::Net::ServerSocket*, const char* msg) override;
		void EventAcceptSocket(const Samurai::IO::Net::ServerSocket*, std::shared_ptr<Samurai::IO::Net::Socket> socket) override;
		void EventGotDatagram(Samurai::IO::Net::DatagramSocket*, Samurai::IO::Net::DatagramPacket*) override;
		void EventDatagramError(const Samurai::IO::Net::DatagramSocket*, const char* msg) override;

	protected:
		Core& core;
		Endpoint at;
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
