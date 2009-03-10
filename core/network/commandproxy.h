/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_COMMANDPROXY_H
#define HAVE_QUICKDC_COMMANDPROXY_H

#include "quickdc.h"
#include <string>
#include <list>
#include <samurai/io/buffer.h>
#include <samurai/util/refcount.h>

#define LOG_NET(prefix, fd, cmd, len) Log_Network_Traffic(prefix, fd, cmd, len);
extern void Log_Network_Traffic(const char* event, int socketFD, const char* command, size_t length);

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
}

namespace QuickDC {

class CommandParser;
class CommandBase;

class CommandProxy {
	public:
		CommandProxy(Samurai::IO::Net::Socket* socket);
		virtual ~CommandProxy();

		/**
		 * Parse and process the available data.
		 */
		virtual void invoke(const char* command, size_t length);

		/**
		 * Read from socket and call the apropriate parser functions.
		 */
		virtual bool read() = 0;
		
		/**
		 * Close the connection
		 */
		virtual void close() = 0;
		
		/**
		 * Returns the number of bytes pending in the send buffer.
		 */
		virtual size_t getSendBufferSize() const { return sendbuffer->size(); }
		
		/**
		 * Returns the number of bytes pending in the receive buffer.
		 */
		virtual size_t getReadBufferSize() const { return sendbuffer->size(); }

	public:
		Samurai::IO::Net::Socket* socket;
		Samurai::IO::Buffer* sendbuffer;
		Samurai::IO::Buffer* readbuffer;
};

class CommandBase : public Samurai::Util::RefCounter
{
	public:

		/**
		 * @return the lenght of the command as it would be presented in the final stream.
		 */
		virtual size_t getSize() = 0;
		
		/**
		 * Writes the command to the given buffer.
		 */
		virtual void write(Samurai::IO::Buffer* stream) = 0;
		
		/**
		 * Returns a char* to the RAW command.
		 * FIXME: API should change.
		 */
		virtual char* getRawCommand() = 0;
};

class QueuedCommand
{
	public:
		QueuedCommand(CommandBase* cmd);
		virtual ~QueuedCommand();
		
		CommandBase* getCommand() { return command; }
		virtual char* getRawCommand();
		virtual size_t getSize();
		
		virtual size_t getOffset();
		virtual void setOffset(size_t offset);
		
	protected:
		CommandBase* command;
		size_t offset;
};

class CommandFIFO
{
	public:
		CommandFIFO();
		virtual ~CommandFIFO();
		
		virtual void lock();
		virtual void unlock();
		
		virtual size_t getQueueSize() const;
		virtual size_t getWaitQueueSize() const;
		virtual QueuedCommand* first();
		virtual void pop();
		virtual void push(CommandBase*);
		
		/**
		 * Flush wait queue into queue.
		 */
		virtual void flush();
		
	protected:
		std::list<QueuedCommand*> queue;
		std::list<QueuedCommand*> wait_queue;
		size_t queue_size;
		size_t wait_queue_size;
		bool locked;
};

}

#endif // HAVE_QUICKDC_COMMANDPROXY_H
