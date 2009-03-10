/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCCOMMANDPROXY_H
#define HAVE_QUICKDC_ADCCOMMANDPROXY_H

#include "quickdc.h"
#include <map>
#include <list>
#include "network/commandproxy.h"

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
}

namespace QuickDC {
class QueuedCommand;
}

namespace ADC {
class ClientSession;
class CommandParser;
class Command;
class HubSession;
class Hub;


class CommandProxy : public QuickDC::CommandProxy
{
	public:
	
		CommandProxy(Samurai::IO::Net::Socket* socket, ADC::ClientSession* session);
		CommandProxy(Samurai::IO::Net::Socket* socket, ADC::HubSession* session);
		CommandProxy(Samurai::IO::Net::Socket* socket, ADC::Hub* hub);
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

		/**
		 * Processors
		 */
		virtual bool read();
		virtual bool write();
		virtual void close();
		virtual void connect();
		void send(ADC::Command* cmd);
	
		size_t getSendQueueSize() const;
	
	protected:
		std::map<uint32_t, ADC::CommandParser*> mapper;
		void clearParsers();
		QuickDC::CommandFIFO queue;
};

}

#endif // HAVE_QUICKDC_ADCCOMMANDPROXY_H
