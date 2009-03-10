/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCCOMMANDPARSER_H
#define HAVE_QUICKDC_DCCOMMANDPARSER_H

#include "network/commandparser.h"

namespace QuickDC {
	class CommandParser;
	class CommandBase;
	class QueuedCommand;
}

namespace DC {

class CommandProxy;
class HubSession;
class ClientSession;

class Command : public QuickDC::CommandBase
{
	public:
		Command();
		virtual ~Command();
		
		size_t getSize();
		void write(Samurai::IO::Buffer* stream);
		char* getRawCommand();
		
	protected:
		Samurai::IO::Buffer* buffer;
		size_t offset;
};

class CommandParser : public QuickDC::CommandParser
{
	public:
		virtual ~CommandParser() { }
		virtual bool invoke(const char* line, size_t length) = 0;
};

class HubCommandParser : public DC::CommandParser
{
	public:
		HubCommandParser(DC::CommandProxy* proxy, DC::HubSession* session, const char* cmd);
		virtual ~HubCommandParser();
		
		virtual bool invoke(const char* line, size_t length) = 0;
	
	protected:
		DC::CommandProxy* proxy;
		DC::HubSession* session;
};

class ClientCommandParser : public DC::CommandParser
{
	public:
		ClientCommandParser(DC::CommandProxy* proxy, DC::ClientSession* session, const char* cmd);
		virtual ~ClientCommandParser();
		
		virtual bool invoke(const char* line, size_t length) = 0;
	
	protected:
		DC::CommandProxy* proxy;
		DC::ClientSession* session;
};

}

#endif // HAVE_QUICKDC_DCCOMMANDPARSER_H
