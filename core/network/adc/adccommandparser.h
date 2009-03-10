/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCCOMMANDPARSER_H
#define HAVE_QUICKDC_DCCOMMANDPARSER_H

#include "network/commandparser.h"
#include "network/adc/client/adchubsession.h"
#include "network/adc/client/adcclientsession.h"


namespace QuickDC {
class CommandParser;
}

namespace ADC {
class Command;
class CommandProxy;
class Hub;

class CommandParser : public QuickDC::CommandParser
{
	public:
		virtual ~CommandParser() { }
		virtual bool invoke(Command*) = 0;
};

class HubCommandParser : public ADC::CommandParser
{
	public:
		HubCommandParser(CommandProxy* proxy, HubSession* session);
		virtual ~HubCommandParser();
		
		virtual bool invoke(Command*) = 0;
		
	protected:
		HubSession* session;
		CommandProxy* proxy;
};

class ClientCommandParser : public ADC::CommandParser
{
	public:
		ClientCommandParser(CommandProxy* proxy, ClientSession* session);
		virtual ~ClientCommandParser();
		
		virtual bool invoke(Command*) = 0;
		
	protected:
		ClientSession* session;
		CommandProxy* proxy;
};

class LocalHubParser : public ADC::CommandParser
{
	public:
		LocalHubParser(CommandProxy* proxy, Hub* hub);
		virtual ~LocalHubParser();
		
		virtual bool invoke(Command*) = 0;
		
	protected:
		Hub* hub;
		CommandProxy* proxy;
};


}

#endif // HAVE_QUICKDC_DCCOMMANDPARSER_H
