/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCCOMMANDPROXY_H
#define HAVE_QUICKDC_DCCOMMANDPROXY_H

#include "network/commandproxy.h"
#include <map>
#include <list>
#include <string_view>

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
}

namespace QuickDC {
class CommandProxy;
}

namespace DC {

struct eqstr {
  bool operator()(const char* s1, const char* s2) const {
    return std::string_view(s1) < std::string_view(s2);
  }
};

class Command;
class CommandParser;
class ClientSession;
class HubSession;

class CommandProxy : public QuickDC::CommandProxy {
	public:
		CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket, DC::ClientSession* session);
		CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket, DC::HubSession* session);
		virtual ~CommandProxy();
		
		/**
		 * Parse and process the available data.
		 */
		virtual void invoke(const char* command, size_t length);
		
		/**
		 * The default command parser (if no valid ones are found).
		 * This is usually called when you receive unimplemented or
		 * wrong commands.
		 */
		virtual void defaultHandler(const char* command);

		void send(Command* cmd);

		/**
		 * Processors
		 */
		virtual bool read();
		virtual bool write(); /* flush? */
		virtual void close();
		virtual void connect();

		/**
		 * Add a command parser
		 */
		void registerParser(const char* name, CommandParser* parser);
	
	protected:
		std::map<const char*, CommandParser*, eqstr> mapper;
		bool client;

		/**
		 * Set by close(), so read() stops dispatching. One packet can hold
		 * several '|' separated commands, and a handler that drops the
		 * connection must not have the rest of them delivered to it.
		 */
		bool closed;

		/**
		 * Close the connection over a command that exceeds
		 * QuickDC::MAX_COMMAND_LINE, and answer read()'s failure.
		 */
		bool dropOverlongLine();

		void clearParsers();

		QuickDC::CommandFIFO queue;
};

}

#endif // HAVE_QUICKDC_DCCOMMANDPROXY_H
