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
#include "network/adc/server/commands.h"
#include "network/adc/parser.h"
#include "network/transfer.h"
#include <map>

#define READ_BUF_SIZE 65536 * 2

#define FOURCC_REG(a, b, c, d, cls) \
	mapper.insert(std::pair<uint32_t, ADC::CommandParser*>(FOURCC(a, b, c, d), cls));

ADC::CommandProxy::CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket, ADC::ClientSession* session) : QuickDC::CommandProxy(std::move(socket)), handover(0)
{
	FOURCC_REG('C','S','U','P', new ADC::CCmdSUP(this, session))
	FOURCC_REG('C','I','N','F', new ADC::CCmdINF(this, session))
	FOURCC_REG('C','G','F','I', new ADC::CCmdGFI(this, session))
	FOURCC_REG('C','G','E','T', new ADC::CCmdGET(this, session))
	FOURCC_REG('C','S','N','D', new ADC::CCmdSND(this, session))
	FOURCC_REG('C','S','T','A', new ADC::CCmdSTA(this, session))
	FOURCC_REG('C','R','E','S', new ADC::CCmdRES(this, session))
}

ADC::CommandProxy::CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket, ADC::HubSession* session) : QuickDC::CommandProxy(std::move(socket)), handover(0)
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
	FOURCC_REG('D','P','S','R', new ADC::CmdPartialStatus(this, session));
	FOURCC_REG('D','C','T','M', new ADC::CmdCTM(this, session));
	FOURCC_REG('D','R','C','M', new ADC::CmdRCTM(this, session));
	FOURCC_REG('D','N','A','T', new ADC::CmdNAT(this, session));
	FOURCC_REG('D','R','N','T', new ADC::CmdRNT(this, session));
	FOURCC_REG('I','B','B','D', new ADC::CmdBoardDesc(this, session));
	FOURCC_REG('I','B','B','L', new ADC::CmdBoardEntry(this, session));
}

ADC::CommandProxy::CommandProxy(std::shared_ptr<Samurai::IO::Net::Socket> socket, ADC::LocalHub::Hub* hub) : QuickDC::CommandProxy(std::move(socket)), handover(0)
{
	// hub messages
	FOURCC_REG('H','S','U','P', new ADC::LocalHub::CmdSUP(this, hub));
	FOURCC_REG('H','P','A','S', new ADC::LocalHub::CmdPAS(this, hub));
	FOURCC_REG('H','D','S','C', new ADC::LocalHub::CmdDSC(this, hub));
	FOURCC_REG('H','M','S','G', new ADC::LocalHub::CmdMSG(this, hub));
	FOURCC_REG('H','B','B','L', new ADC::LocalHub::CmdBBL(this, hub));
	FOURCC_REG('H','B','B','P', new ADC::LocalHub::CmdBBP(this, hub));

	// broadcast messages
	FOURCC_REG('B','I','N','F', new ADC::LocalHub::CmdINF(this, hub));
	FOURCC_REG('B','M','S','G', new ADC::LocalHub::CmdMSG(this, hub));
	FOURCC_REG('B','S','C','H', new ADC::LocalHub::CmdSCH(this, hub));

	// direct messages
	FOURCC_REG('D','M','S','G', new ADC::LocalHub::CmdMSG(this, hub));
	FOURCC_REG('D','S','C','H', new ADC::LocalHub::CmdSCH(this, hub));
	FOURCC_REG('D','R','E','S', new ADC::LocalHub::CmdRES(this, hub));
	FOURCC_REG('D','C','T','M', new ADC::LocalHub::CmdCTM(this, hub));
	FOURCC_REG('D','R','C','M', new ADC::LocalHub::CmdRCM(this, hub));
	FOURCC_REG('D','N','A','T', new ADC::LocalHub::CmdNAT(this, hub));
	FOURCC_REG('D','R','N','T', new ADC::LocalHub::CmdRNT(this, hub));
	FOURCC_REG('D','S','T','A', new ADC::LocalHub::CmdSTA(this, hub));

	// echo messages
	FOURCC_REG('E','M','S','G', new ADC::LocalHub::CmdMSG(this, hub));
	FOURCC_REG('E','S','C','H', new ADC::LocalHub::CmdSCH(this, hub));

	// feature messages
	FOURCC_REG('F','S','C','H', new ADC::LocalHub::CmdSCH(this, hub));
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
	cmd->setSocket(socket.get());
	
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
		const char* command = qcmd->getRawCommand();
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
		if (static_cast<size_t>(sent) < toSend)
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

/*
 * Neither ADC nor the buffer holding it bounds a line, and the buffer doubles as
 * it grows - so without a ceiling a peer decides how much memory we hold on its
 * behalf simply by never sending a newline.
 */
bool ADC::CommandProxy::dropOverlongLine()
{
	QERR("ADC command line longer than %zu bytes; closing connection",
		QuickDC::MAX_COMMAND_LINE);
	readbuffer->clear();
	close();
	return false;
}

bool ADC::CommandProxy::read()
{
	/* Per thread: two cores on two threads read at once, and shared scratch hands one the other's bytes. */
	thread_local char buf[READ_BUF_SIZE+1] = {0,};
	size_t n = 0;
	std::error_code ec;
	switch (socket->read(buf, READ_BUF_SIZE, n, ec))
	{
		case Samurai::IO::ReadResult::Ok:
			break;

		/* Nothing to parse; the socket reports again when there is. */
		case Samurai::IO::ReadResult::WouldBlock:
		case Samurai::IO::ReadResult::EndOfFile:
			return true;

		case Samurai::IO::ReadResult::Error:
			return false;
	}
	readbuffer->append(buf, n);
	size_t split = readbuffer->find('\n');
	while (split != Samurai::IO::Buffer::npos)
	{
		if (!split)
		{
			readbuffer->remove(1);
			split = readbuffer->find('\n');
			continue;
		}

		/* Refused before it is parsed, so the offending bytes are never acted on. */
		if (split > QuickDC::MAX_COMMAND_LINE)
			return dropOverlongLine();

		std::string command = readbuffer->copyRange(0, split+1);
		readbuffer->remove(split+1);
		invoke(command.c_str(), split);

		if (handover) break;

		split = readbuffer->find('\n');
	}

	/* Nothing has terminated the line that is still buffered, and it is already
	   longer than one may be. */
	if (!handover && readbuffer->size() > QuickDC::MAX_COMMAND_LINE)
		return dropOverlongLine();

	if (handover)
	{
		QuickDC::Download* download = handover;
		handover = 0;

		size_t left = readbuffer->size();
		if (left)
		{
			const std::string data = readbuffer->copyRange(0, left);
			readbuffer->remove(left);
			download->pushPending(data.c_str(), left);

			/* A small chunk may be here in full, with no further read event
			   coming. Only safe with data in hand, since the download then
			   returns without reading. May finish and delete the transfer. */
			download->exec();
		}
	}

	return true;
}

void ADC::CommandProxy::handOverTo(QuickDC::Download* download)
{
	handover = download;
}

bool ADC::CommandProxy::send(Command* cmd)
{
	return queue.push(cmd);
}

void ADC::CommandProxy::setSendQueueLimit(size_t bytes)
{
	queue.setMaxQueueSize(bytes);
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
	QDBG("Unhandled input '%s', length=%zu", command, strlen(command));
	(void) command;
}

size_t ADC::CommandProxy::getSendQueueSize() const
{
	return queue.getQueueSize() + queue.getWaitQueueSize();
}

