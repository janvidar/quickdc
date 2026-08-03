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
class Download;
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
	
		CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket, ADC::ClientSession* session);
		CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket, ADC::HubSession* session);
		CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket, ADC::Hub* hub);
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

		/**
		 * Queue a command for sending.
		 * @return false if the send queue ceiling was reached and the command
		 *         was dropped.
		 */
		bool send(ADC::Command* cmd);

		size_t getSendQueueSize() const;

		/** The most the send queue may hold; 0 for no ceiling. */
		void setSendQueueLimit(size_t bytes);

		/**
		 * Give the rest of the connection over to a download; what is still
		 * buffered is file data and must not be parsed as commands.
		 */
		void handOverTo(QuickDC::Download* download);

	protected:
		/**
		 * Close the connection over a line that exceeds
		 * QuickDC::MAX_COMMAND_LINE, and answer read()'s failure.
		 */
		bool dropOverlongLine();

		std::map<uint32_t, ADC::CommandParser*> mapper;
		void clearParsers();
		QuickDC::CommandFIFO queue;
		QuickDC::Download* handover;
};

}

#endif // HAVE_QUICKDC_ADCCOMMANDPROXY_H
