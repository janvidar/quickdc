/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <samurai/debug/dbg.h>
#include <samurai/io/buffer.h>
#include <samurai/io/net/socket.h>
#include "network/commandproxy.h"
#include "network/commandparser.h"

void Log_Network_Traffic(const char* event, int socketFD, const char* command, size_t length)
{
	/*
	 * The copy exists only to terminate what QNET prints, and this runs once per
	 * command in each direction - so with the tracing subsystem not compiled in
	 * there is an allocation and a free per packet to no end.
	 */
#ifdef DEBUG
	char* command_str = strndup(command, length);
	QNET("%s[%d]: '%s'", event, socketFD, command_str);
	free(command_str);
#else
	(void) event;
	(void) socketFD;
	(void) command;
	(void) length;
#endif
}


QuickDC::CommandProxy::CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket_) : socket(std::move(socket_))
{
	sendbuffer = new Samurai::IO::Buffer();
	readbuffer = new Samurai::IO::Buffer();
}

QuickDC::CommandProxy::~CommandProxy()
{
	delete sendbuffer;
	delete readbuffer;
}

void QuickDC::CommandProxy::invoke(const char*, size_t)
{

}

bool QuickDC::CommandProxy::discard()
{
	/*
	 * Until the socket says there is no more, rather than one read and out: a peer
	 * sending faster than one buffer per pass would otherwise keep the descriptor
	 * readable no matter how often this ran.
	 */
	for (;;)
	{
		char scratch[4096];
		size_t got = 0;
		std::error_code ec;

		switch (socket->read(scratch, sizeof(scratch), got, ec))
		{
			case Samurai::IO::ReadResult::Ok:
				if (!got) return true;
				break;

			case Samurai::IO::ReadResult::WouldBlock:
				return true;

			case Samurai::IO::ReadResult::EndOfFile:
			case Samurai::IO::ReadResult::Error:
				return false;
		}
	}
}

QuickDC::QueuedCommand::QueuedCommand(QuickDC::CommandBase* cmd)
{
	command = cmd;
	command->incRef();
	offset = 0;
}

QuickDC::QueuedCommand::~QueuedCommand()
{
	command->decRef();
	if (command->canDelete())
	{
		delete command;
	}
}

const char* QuickDC::QueuedCommand::getRawCommand()
{
	return command->getRawCommand();
}

size_t QuickDC::QueuedCommand::getSize()
{
	return command->getSize();
}

size_t QuickDC::QueuedCommand::getOffset()
{
	return offset;
}

void QuickDC::QueuedCommand::setOffset(size_t offset_)
{
	offset = offset_;
}

QuickDC::CommandFIFO::CommandFIFO()
	: queue_size(0)
	, wait_queue_size(0)
	, max_queue_size(0)
	, locked(false)
{
}

QuickDC::CommandFIFO::~CommandFIFO()
{
	while (queue.size())
	{
		QuickDC::QueuedCommand* cmd = queue.front();
		queue.pop_front();
		delete cmd;
	}
	
	while (wait_queue.size())
	{
		QuickDC::QueuedCommand* cmd = wait_queue.front();
		wait_queue.pop_front();
		delete cmd;
	}

}

void QuickDC::CommandFIFO::lock()
{
	locked = true;
}

void QuickDC::CommandFIFO::unlock()
{
	locked = false;
}

void QuickDC::CommandFIFO::flush()
{
	// Empty the wating queue, and fill into the real queue.
	while (wait_queue.size())
	{
		QuickDC::QueuedCommand* qcmd = wait_queue.front();
		wait_queue.pop_front();
		
		queue_size += qcmd->getSize();
		wait_queue_size -= qcmd->getSize();
		queue.push_back(qcmd);
	}
}

void QuickDC::CommandFIFO::pop()
{
	if (!queue.size())
		return;
	
	QuickDC::QueuedCommand* qcmd = queue.front();
	queue.pop_front();
	queue_size -= qcmd->getSize();
}

QuickDC::QueuedCommand* QuickDC::CommandFIFO::first()
{
	if (!queue.size())
		return 0;
	
	QuickDC::QueuedCommand* qcmd = queue.front();
	return qcmd;
}

bool QuickDC::CommandFIFO::push(QuickDC::CommandBase* cmd)
{
	/*
	 * Measured before the command is taken over, so that a refusal leaves the
	 * caller's reference count exactly as it found it.
	 */
	if (max_queue_size && queue_size + wait_queue_size + cmd->getSize() > max_queue_size)
		return false;

	QuickDC::QueuedCommand* qcmd = new QuickDC::QueuedCommand(cmd);

	if (!locked)
	{
		queue.push_back(qcmd);
		queue_size += qcmd->getSize();
	}
	else
	{
		wait_queue.push_back(qcmd);
		wait_queue_size += qcmd->getSize();
	}
	return true;
}

size_t QuickDC::CommandFIFO::getQueueSize() const
{
	return queue_size;
}

size_t QuickDC::CommandFIFO::getWaitQueueSize() const
{
	return wait_queue_size;
}
