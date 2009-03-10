/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include "network/commandparser.h"
#include "network/commandproxy.h"
#include "network/adc/adccommandparser.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/server/adchub.h"
#include <stdio.h>

ADC::HubCommandParser::HubCommandParser(ADC::CommandProxy* proxy_, ADC::HubSession* session_)
	: session(session_)
	, proxy(proxy_)
{

}

ADC::HubCommandParser::~HubCommandParser()
{

}

bool ADC::HubCommandParser::invoke(ADC::Command*)
{
	QDBG("HubCommandParser::invoke(): Not invented here!");
	return false;
}

ADC::ClientCommandParser::ClientCommandParser(ADC::CommandProxy* proxy_, ADC::ClientSession* session_)
	: session(session_)
	, proxy(proxy_)
{

}

ADC::ClientCommandParser::~ClientCommandParser()
{

}

bool ADC::ClientCommandParser::invoke(ADC::Command*)
{
	QDBG("HubCommandParser::invoke(): Not invented here!");
	return false;
}

ADC::LocalHubParser::LocalHubParser(CommandProxy* proxy_, Hub* hub_)
	: hub(hub_)
	, proxy(proxy_)
{

}

ADC::LocalHubParser::~LocalHubParser()
{

}

bool ADC::LocalHubParser::invoke(ADC::Command*)
{
	QDBG("ADC::LocalHubParser::invoke(): Not invented here!");
	return false;
}
