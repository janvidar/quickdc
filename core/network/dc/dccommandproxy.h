/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCCOMMANDPROXY_H
#define HAVE_QUICKDC_DCCOMMANDPROXY_H

#include "network/commandproxy.h"
#include <map>
#include <list>

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
  	bool hit = strcmp(s1, s2) < 0;
    return hit;
  }
};

class Command;
class CommandParser;
class ClientSession;
class HubSession;

class CommandProxy : public QuickDC::CommandProxy {
	public:
		CommandProxy(Samurai::IO::Net::Socket* socket, DC::ClientSession* session);
		CommandProxy(Samurai::IO::Net::Socket* socket, DC::HubSession* session);
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
		void clearParsers();
		
		QuickDC::CommandFIFO queue;
};

}

#endif // HAVE_QUICKDC_DCCOMMANDPROXY_H
