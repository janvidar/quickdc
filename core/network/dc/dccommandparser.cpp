/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/dc/dccommandparser.h"
#include "network/commandparser.h"
#include "network/commandproxy.h"
#include "network/dc/dccommandproxy.h"
#include <stdio.h>


DC::Command::Command()
{
	buffer = new Samurai::IO::Buffer();
	offset = 0;
}

DC::Command::~Command()
{
	delete buffer;
}

size_t DC::Command::getSize()
{
	return buffer->size();
}

void DC::Command::write(Samurai::IO::Buffer* stream)
{
	stream->append(buffer);
}

char* DC::Command::getRawCommand()
{
	return buffer->ptr();
}

DC::HubCommandParser::HubCommandParser(DC::CommandProxy* proxy_, DC::HubSession* session_, const char* cmd) : proxy(proxy_), session(session_)
{
	proxy->registerParser(cmd, this);
}

DC::HubCommandParser::~HubCommandParser()
{
}

bool DC::HubCommandParser::invoke(const char* /*line*/, size_t /*length*/)
{
	return false;
}

DC::ClientCommandParser::ClientCommandParser(DC::CommandProxy* proxy_, DC::ClientSession* session_, const char* cmd) : proxy(proxy_), session(session_)
{
	proxy->registerParser(cmd, this);
}

DC::ClientCommandParser::~ClientCommandParser()
{
}

bool DC::ClientCommandParser::invoke(const char* /*line*/, size_t /*length*/)
{
	return false;
}



