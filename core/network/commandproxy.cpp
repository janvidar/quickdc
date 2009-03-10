/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <samurai/debug/dbg.h>
#include <samurai/io/buffer.h>
#include "network/commandproxy.h"
#include "network/commandparser.h"

void Log_Network_Traffic(const char* event, int socketFD, const char* command, size_t length)
{
	char* command_str = strndup(command, length);
	QNET("%s[%d]: '%s'", event, socketFD, command_str);
	free(command_str); // FIXME: Remove this!
}


QuickDC::CommandProxy::CommandProxy(Samurai::IO::Net::Socket* socket_) : socket(socket_)
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

char* QuickDC::QueuedCommand::getRawCommand()
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

void QuickDC::CommandFIFO::push(QuickDC::CommandBase* cmd)
{
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
}

size_t QuickDC::CommandFIFO::getQueueSize() const
{
	return queue_size;
}

size_t QuickDC::CommandFIFO::getWaitQueueSize() const
{
	return wait_queue_size;
}
