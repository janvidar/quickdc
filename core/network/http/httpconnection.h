/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_CONNECTION_H
#define HAVE_QUICKDC_HTTP_CONNECTION_H

#include "network/connection.h"
#include "network/connectionmanager.h"
#include "network/commandproxy.h"
#include <samurai/io/net/socket.h>

namespace Samurai {
	namespace IO {
		class Buffer;
		namespace Net {
			class Socket;
		}
	}
}

namespace QuickDC {
	class ConnectionManager;
	class Connection;
	class CommandProxy;
}

namespace Http {

class Parser;
class Pipeline;

class Connection :
	public QuickDC::Connection,
	public QuickDC::CommandProxy
{
	public:
		Connection(Samurai::IO::Net::Socket* sock, bool client);
		virtual ~Connection();
		
		enum ParseMode { None, Header, Body };

		void close();

		/**
		 * Make sure the pipeline is being processed,
		 *
		 */
		void processPipeline();
		
		/**
		 * Is this connection a client connection?
		 */
		bool isClient() const { return client; }
		
		/**
		 * Is this connection a server connection?
		 */
		bool isServer() const { return !client; }

	protected:
		bool read();
		bool write();

	protected:
		virtual void EventHostLookup(const Samurai::IO::Net::Socket*);
		virtual void EventHostFound(const Samurai::IO::Net::Socket*);
		virtual void EventConnecting(const Samurai::IO::Net::Socket*);
		virtual void EventConnected(const Samurai::IO::Net::Socket*);
		virtual void EventTimeout(const Samurai::IO::Net::Socket*);
		virtual void EventDisconnected(const Samurai::IO::Net::Socket*);
		virtual void EventDataAvailable(const Samurai::IO::Net::Socket*);
		virtual void EventCanWrite(const Samurai::IO::Net::Socket*);
		virtual void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char* msg);

		void generateErrorPage(Samurai::IO::Buffer* buffer, const char* title, const char* message);

	protected:
		bool client;
		Http::Parser* parser_in;
		Http::Parser* parser_out;
		Http::Pipeline* pipeline_in;
		Http::Pipeline* pipeline_out;

};

}

#endif // HAVE_QUICKDC_HTTP_CONNECTION_H
