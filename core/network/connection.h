/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONNECTION_H
#define HAVE_QUICKDC_CONNECTION_H

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

class Connection : public Samurai::IO::Net::SocketEventHandler
{
	public:
		Connection(Samurai::IO::Net::Socket* sock, const char* protocol);
		Connection(Samurai::IO::Net::URL* url, const char* protocol);
		virtual ~Connection();
		
		void setManager(ConnectionManager* mgr) { manager = mgr; }
		
		Samurai::IO::Net::Socket* getSocket() { return socket; }

		/**
		 * @return the name of the current protocol.
		 */
		const char* getProtocol() const { return protocol; }
		
	protected:
		virtual void EventHostLookup(const Samurai::IO::Net::Socket*) { };
		virtual void EventHostFound(const Samurai::IO::Net::Socket*) { };
		virtual void EventConnecting(const Samurai::IO::Net::Socket*) { };
		virtual void EventConnected(const Samurai::IO::Net::Socket*) = 0;
		virtual void EventTimeout(const Samurai::IO::Net::Socket*) = 0;
		virtual void EventDisconnected(const Samurai::IO::Net::Socket*) = 0;
		virtual void EventDataAvailable(const Samurai::IO::Net::Socket*) = 0;
		virtual void EventCanWrite(const Samurai::IO::Net::Socket*) = 0;
		virtual void EventError(const Samurai::IO::Net::Socket*, enum Samurai::IO::Net::SocketError error, const char*) = 0;
	
	protected:
		Samurai::IO::Net::Socket* socket;
		ConnectionManager* manager;
		const char* protocol;
};

}

#endif // HAVE_QUICKDC_CONNECTION_H
