/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <samurai/io/net/socket.h>
#include "network/commandparser.h"
#include "network/adc/adccommandparser.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/adccommands.h"
#include "network/adc/parser.h"
#include <map>

#define READ_BUF_SIZE 65536 * 2

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

#define FOURCC_REG(a, b, c, d, cls) \
	mapper.insert(std::pair<uint32_t, ADC::CommandParser*>( \
	((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d)), cls));

ADC::CommandProxy::CommandProxy(Samurai::IO::Net::Socket* socket, ADC::ClientSession* session) : QuickDC::CommandProxy(socket)
{
	FOURCC_REG('C','S','U','P', new ADC::CCmdSUP(this, session))
	FOURCC_REG('C','I','N','F', new ADC::CCmdINF(this, session))
	FOURCC_REG('C','G','F','I', new ADC::CCmdGFI(this, session))
	FOURCC_REG('C','G','E','T', new ADC::CCmdGET(this, session))
	FOURCC_REG('C','S','N','D', new ADC::CCmdSND(this, session))
	FOURCC_REG('C','S','T','A', new ADC::CCmdSTA(this, session))
	FOURCC_REG('C','R','E','S', new ADC::CCmdRES(this, session))
}

ADC::CommandProxy::CommandProxy(Samurai::IO::Net::Socket* socket, ADC::HubSession* session) : QuickDC::CommandProxy(socket)
{
	FOURCC_REG('I','S','I','D', new ADC::CmdSID(this, session));
	FOURCC_REG('I','I','N','F', new ADC::CmdInfo(this, session));
	FOURCC_REG('B','I','N','F', new ADC::CmdInfo(this, session));
	FOURCC_REG('I','S','T','A', new ADC::CmdStatus(this, session));
	FOURCC_REG('D','S','T','A', new ADC::CmdStatus(this, session));
	FOURCC_REG('I','Q','U','I', new ADC::CmdQuit(this, session));
	FOURCC_REG('I','G','P','A', new ADC::CmdGetPass(this, session));
	FOURCC_REG('I','S','U','P', new ADC::CmdSupport(this, session));
	FOURCC_REG('B','M','S','G', new ADC::CmdMessage(this, session));
	FOURCC_REG('D','M','S','G', new ADC::CmdMessage(this, session));
	FOURCC_REG('E','M','S','G', new ADC::CmdMessage(this, session));
	FOURCC_REG('I','M','S','G', new ADC::CmdMessage(this, session));
	FOURCC_REG('B','S','C','H', new ADC::CmdSearch(this, session));
	FOURCC_REG('D','S','C','H', new ADC::CmdSearch(this, session));
	FOURCC_REG('F','S','C','H', new ADC::CmdSearch(this, session));
	FOURCC_REG('D','R','E','S', new ADC::CmdResult(this, session));
	FOURCC_REG('D','C','T','M', new ADC::CmdCTM(this, session));
	FOURCC_REG('D','R','C','M', new ADC::CmdRCTM(this, session));
}

ADC::CommandProxy::CommandProxy(Samurai::IO::Net::Socket* socket, ADC::Hub* hub) : QuickDC::CommandProxy(socket)
{
	// hub messages
	FOURCC_REG('H','S','U','P', new ADC::HubCmdSUP(this, hub));
	FOURCC_REG('H','P','A','S', new ADC::HubCmdPAS(this, hub));
	FOURCC_REG('H','D','S','C', new ADC::HubCmdDSC(this, hub));
	FOURCC_REG('H','M','S','G', new ADC::HubCmdMSG(this, hub));

	// broadcast messages
	FOURCC_REG('B','I','N','F', new ADC::HubCmdINF(this, hub));
	FOURCC_REG('B','M','S','G', new ADC::HubCmdMSG(this, hub));
	FOURCC_REG('B','S','C','H', new ADC::HubCmdSCH(this, hub));

	// direct messages
	FOURCC_REG('D','M','S','G', new ADC::HubCmdMSG(this, hub));
	FOURCC_REG('D','S','C','H', new ADC::HubCmdSCH(this, hub));
	FOURCC_REG('D','R','E','S', new ADC::HubCmdRES(this, hub));
	FOURCC_REG('D','C','T','M', new ADC::HubCmdCTM(this, hub));
	FOURCC_REG('D','R','C','M', new ADC::HubCmdRCM(this, hub));
	FOURCC_REG('D','S','T','A', new ADC::HubCmdSTA(this, hub));

	// echo messages
	FOURCC_REG('E','M','S','G', new ADC::HubCmdMSG(this, hub));
	FOURCC_REG('E','S','C','H', new ADC::HubCmdSCH(this, hub));

	// feature messages
	FOURCC_REG('F','S','C','H', new ADC::HubCmdSCH(this, hub));
}

ADC::CommandProxy::~CommandProxy()
{
	while (mapper.size())
	{
		QuickDC::CommandParser* parser = (*mapper.begin()).second;
		mapper.erase(mapper.begin());
		delete parser;
	}
}

void ADC::CommandProxy::invoke(const char* command, size_t length)
{
	LOG_NET("recv", socket->getFD(), command, length);
	ADC::Command* cmd = new ADC::Command();
	bool ok = ADC::Parser::parse(command, length, *cmd);
	cmd->incRef();
	cmd->setSocket(socket);
	
	if (!ok || mapper.count(cmd->getFourcc()) == 0 || !mapper[cmd->getFourcc()]->invoke(cmd))
	{
		defaultHandler(command);
	}
	
	cmd->decRef();
	if (cmd->canDelete())
		delete cmd;
}

// commit write operation
bool ADC::CommandProxy::write()
{
	queue.lock();
	
	bool ok = true;
	bool more = true;
	while (more && queue.getQueueSize())
	{
		QuickDC::QueuedCommand* qcmd = queue.first();
		char*  command = qcmd->getRawCommand();
		size_t size    = qcmd->getSize();
		size_t offset  = qcmd->getOffset();
		size_t toSend  = size-offset;

		LOG_NET("send", socket->getFD(), command, toSend);
		
		ssize_t sent = socket->write(&command[offset], toSend);
		
		if (sent == -1)
		{
			ok = false;
			more = false;
			break;
		}
	
		qcmd->setOffset(offset + sent);
	
		// FIXME: toSend
		if ((size_t) sent < toSend)
		{
			socket->toggleWriteNotifier(true);
			more = false;
		}
		else
		{
			queue.pop();
			delete qcmd;
		}
	}

	if (ok)
	{
		if (!queue.getQueueSize())
		{
			socket->toggleWriteNotifier(false);
		}
		
		queue.flush();
	}
	
	queue.unlock();
	return ok;
}

bool ADC::CommandProxy::read()
{
	static char buf[READ_BUF_SIZE+1] = {0,};
	int n = socket->read(buf, READ_BUF_SIZE);
	if (n == -1)
	{
		return false;
	}
	readbuffer->append(buf, n);
	int split = readbuffer->find('\n');
	while (split >= 0)
	{
		if (!split)
		{
			readbuffer->remove(1);
			split = readbuffer->find('\n');
			continue;
		}
		char* command = readbuffer->memdup(0, split+1);
		readbuffer->remove(split+1);
		invoke(command, split);
		free(command);
		
		split = readbuffer->find('\n');
	}
	return true;
}

void ADC::CommandProxy::send(Command* cmd)
{
	queue.push(cmd);
}

void ADC::CommandProxy::close()
{
#ifdef DATADUMP
		QDBG("closing socket'");
#endif
	socket->disconnect();
}

void ADC::CommandProxy::connect()
{
	socket->connect();
}

void ADC::CommandProxy::defaultHandler(const char* command)
{
	QDBG("Unhandled input '%s', length=%d", command, strlen(command));
}

size_t ADC::CommandProxy::getSendQueueSize() const
{
	return queue.getQueueSize() + queue.getWaitQueueSize();
}

