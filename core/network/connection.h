/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONNECTION_H
#define HAVE_QUICKDC_CONNECTION_H

#include <memory>
#include <string>
#include <samurai/io/net/socketevent.h>

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
			class URL;
		}
	}
}

namespace QuickDC {
class ConnectionManager;
class Core;

class Connection : public Samurai::IO::Net::SocketEventHandler
{
	public:
		Connection(Core& core, std::shared_ptr<Samurai::IO::Net::Socket> sock, const char* protocol);
		Connection(Core& core, Samurai::IO::Net::URL* url, const char* protocol);
		virtual ~Connection();

		Connection(const Connection&) = delete;
		Connection& operator=(const Connection&) = delete;
		
		void setManager(ConnectionManager* mgr) { manager = mgr; }

		/**
		 * The manager responsible for deleting this connection, or 0 once it
		 * has been handed over for deletion.
		 */
		ConnectionManager* getManager() const { return manager; }

		Samurai::IO::Net::Socket* getSocket() { return socket.get(); }
		Core& getCore() const { return core; }

		/**
		 * @return the name of the current protocol.
		 */
		const char* getProtocol() const { return protocol; }

		/** Who is at the other end, as far as is known: a peer's CID or nickname, a hub's address. */
		virtual std::string peerName() const { return std::string(); }
		
	protected:
		virtual void EventHostLookup(const Samurai::IO::Net::Socket*) override { };
		virtual void EventHostFound(const Samurai::IO::Net::Socket*) override { };
		virtual void EventConnecting(const Samurai::IO::Net::Socket*) override { };
		virtual void EventConnected(const Samurai::IO::Net::Socket*) override = 0;
		virtual void EventTimeout(const Samurai::IO::Net::Socket*) override = 0;
		virtual void EventDisconnected(const Samurai::IO::Net::Socket*) override = 0;
		virtual void EventDataAvailable(const Samurai::IO::Net::Socket*) override = 0;
		virtual void EventCanWrite(const Samurai::IO::Net::Socket*) override = 0;
		virtual void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError error, const char*) override = 0;
	
	protected:
		Core& core;
		std::shared_ptr<Samurai::IO::Net::Socket> socket;
		ConnectionManager* manager;
		const char* protocol;
};

}

#endif // HAVE_QUICKDC_CONNECTION_H
