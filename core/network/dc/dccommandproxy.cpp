/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <map>

#include "quickdc.h"
#include "network/commandparser.h"
#include "network/dc/hubcommands.h"
#include "network/dc/clientcommands.h"
#include "network/dc/dccommandparser.h"
#include "network/dc/dccommandproxy.h"
#include <samurai/io/net/socket.h>

#define READ_BUF_SIZE 65536

DC::CommandProxy::CommandProxy(Samurai::IO::Net::Socket* socket, DC::ClientSession* session) : QuickDC::CommandProxy(socket), client(true)
{
	new DC::ClientKey(this, session);
	new DC::ClientGet(this, session);
	new DC::ClientLock(this, session);
	new DC::ClientSend(this, session);
	new DC::ClientError(this, session);
	new DC::ClientFailed(this, session);
	new DC::ClientMyNick(this, session);
	new DC::ClientSupports(this, session);
	new DC::ClientMaxedOut(this, session);
	new DC::ClientDirection(this, session);
	new DC::ClientExtADCGET(this, session);
	new DC::ClientExtADCSND(this, session);
	new DC::ClientFileLength(this, session);
	new DC::ClientGetListLen(this, session);
	new DC::ClientExtGetZBlock(this, session);
}

DC::CommandProxy::CommandProxy(Samurai::IO::Net::Socket* socket, DC::HubSession* session) : QuickDC::CommandProxy(socket), client(false)
{
	new DC::To(this, session);
	new DC::Quit(this, session);
	new DC::Hello(this, session);
	new DC::UserIP(this, session);
	new DC::Search(this, session);
	new DC::MyINFO(this, session);
	new DC::OpList(this, session);
	new DC::HubName(this, session);
	new DC::GetPass(this, session);
	new DC::BadPass(this, session);
	new DC::LoggedIn(this, session);
	new DC::NickList(this, session);
	new DC::Supports(this, session);
	new DC::ForceMove(this, session);
	new DC::HubIsFull(this, session);
	new DC::LockParser(this, session);
	new DC::PublicChat(this, session);
	new DC::ActionChat(this, session);
	new DC::UserCommand(this, session);
	new DC::ConnectToMe(this, session);
	new DC::ValidateDenied(this, session);
	new DC::RevConnectToMe(this, session);
}


DC::CommandProxy::~CommandProxy() {
	clearParsers();
}

/**
 * This will parse the connection stream from 
 * a DC server (either HUB or p2p connection) and
 * figure out what is a command, and what is not. 
 * A command is handed off to the apropriate parser,
 * that will take care of the details of it.
 */ 
void DC::CommandProxy::invoke(const char* command, size_t length) {
	QNET("recv[%d]: '%s'", socket->getFD(), command)

	/**
	 * In the command line, isolate the command name 
	 * and lookup a handler for it.
	 */
	size_t sep = 0;
	for (size_t i = 0; i < length; i++) {
		if (command[i] == ' ') {
			sep = i;
			break;
		}
	}
	if (sep == 0) sep = length;
	if (sep == 0) return;

	/**
	 * Call the command handler, or the fallback handler
	 * if one doesn't exist.
	 */
	char* cmd = strndup(command, sep);
	
	if (mapper.count(cmd) > 0) {
		if (!mapper[cmd]->invoke(command, length)) {
			defaultHandler(command);
		}
		
	} else {
		// check for chat, which is no command, but a special case.
		if (!client) {
			if (cmd[0] == '<') mapper["PublicChat"]->invoke(command, length);
			else if (cmd[0] == '*') mapper["ActionChat"]->invoke(command, length);
			else defaultHandler(cmd);
		} else {
			defaultHandler(cmd);
		}
	}
	free(cmd);
}

void DC::CommandProxy::send(Command* cmd)
{
	queue.push(cmd);
}

bool DC::CommandProxy::write()
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

bool DC::CommandProxy::read()
{
	static char buf[READ_BUF_SIZE+1] = {0,};
	int n = socket->read(buf, READ_BUF_SIZE);
	if (n == -1)
	{
		return false;
	}
	readbuffer->append(buf, n);
	int split = readbuffer->find('|');
	while (split >= 0)
	{
		if (!split)
		{
			readbuffer->remove(1);
			split = readbuffer->find('|');
			continue;
		}

		char* command = readbuffer->memdup(0, split+1);
		readbuffer->remove(split+1);
		command[split] = 0; // remove pipe
		invoke(command, split);
		free(command);
		
		split = readbuffer->find('|');
	}
	return true;
}


void DC::CommandProxy::close() {
	QDBG("DC::CommandProxy() - closing socket (remote=%s:%d)'", socket->getAddress(), socket->getPort());
	socket->disconnect();
}

void DC::CommandProxy::connect() {
	socket->connect();
}

void DC::CommandProxy::defaultHandler(const char* command) {
	QDBG("UNHANDLED: '%s'", command);
}


void DC::CommandProxy::registerParser(const char* cmd, DC::CommandParser* parser) {
	mapper.insert(std::pair<const char*, DC::CommandParser*>(cmd, parser));
}

void DC::CommandProxy::clearParsers() {
	while (mapper.size()) {
		DC::CommandParser* parser = (*mapper.begin()).second;
		mapper.erase(mapper.begin());
		delete parser;
	}
}

