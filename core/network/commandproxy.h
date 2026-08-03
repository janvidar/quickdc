/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_COMMANDPROXY_H
#define HAVE_QUICKDC_COMMANDPROXY_H

#include "quickdc.h"
#include <memory>
#include <string>
#include <list>
#include <samurai/io/buffer.h>

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

/**
 * The longest a single protocol line may grow before the connection is dropped.
 *
 * Neither ADC nor NMDC bounds a line, so without this a peer that never sends a
 * terminator has the receive buffer doubled for it until the allocation fails.
 * Nothing either protocol carries comes close to this.
 */
inline constexpr size_t MAX_COMMAND_LINE = 16 * 1024;

class CommandProxy {
	public:
		CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket);
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
		std::shared_ptr<Samurai::IO::Net::Socket> socket;
		Samurai::IO::Buffer* sendbuffer;
		Samurai::IO::Buffer* readbuffer;
};

/**
 * Commands are handed around between the proxy that parsed them, the send
 * queue and whatever holds on to them afterwards, so each holder counts its
 * own reference and the last one out deletes.
 */
class CommandBase
{
	public:
		virtual ~CommandBase() = default;

		void incRef() { references++; }
		void decRef() { references--; }
		bool canDelete() const { return !references; }
		size_t getRef() const { return references; }

		/**
		 * @return the lenght of the command as it would be presented in the final stream.
		 */
		virtual size_t getSize() = 0;
		
		/**
		 * Writes the command to the given buffer.
		 */
		virtual void write(Samurai::IO::Buffer* stream) = 0;
		
		/**
		 * Returns the RAW command, owned by the command and valid until it is
		 * modified or destroyed.
		 */
		virtual const char* getRawCommand() = 0;

	private:
		size_t references = 0;
};

class QueuedCommand
{
	public:
		QueuedCommand(CommandBase* cmd);
		virtual ~QueuedCommand();
		
		CommandBase* getCommand() { return command; }
		virtual const char* getRawCommand();
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

		/**
		 * Queue a command.
		 *
		 * @return false when the ceiling set by setMaxQueueSize() would be
		 *         exceeded, in which case the command is not queued. A peer that
		 *         never reads must not be able to make us hold an unbounded
		 *         amount of data on its behalf.
		 */
		virtual bool push(CommandBase*);

		/**
		 * The most this queue may hold, counting what waits behind a lock.
		 * 0 for no ceiling, which is the default.
		 */
		virtual void setMaxQueueSize(size_t bytes) { max_queue_size = bytes; }
		virtual size_t getMaxQueueSize() const { return max_queue_size; }
		
		/**
		 * Flush wait queue into queue.
		 */
		virtual void flush();
		
	protected:
		std::list<QueuedCommand*> queue;
		std::list<QueuedCommand*> wait_queue;
		size_t queue_size;
		size_t wait_queue_size;
		size_t max_queue_size;
		bool locked;
};

}

#endif // HAVE_QUICKDC_COMMANDPROXY_H
