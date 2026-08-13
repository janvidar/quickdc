/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCCOMMANDS_H
#define HAVE_QUICKDC_ADCCOMMANDS_H

#include "network/adc/adccommandparser.h"
#include "network/adc/client/adchubsession.h"
#include "network/adc/parser.h"
#include "network/commandparser.h"

namespace ADC {

class HubSession;
class CommandProxy;
class Command;

void addSupportFlags(ADC::Command& command, bool client);

#define ADC_HUB_CMD_CLASS(N) \
	class N : public ADC::HubCommandParser { \
		public: \
			N(ADC::CommandProxy* proxy, ADC::HubSession* session) : ADC::HubCommandParser(proxy, session) { }; \
			bool invoke(ADC::Command*) override; \
	};


#define ADC_CLIENT_CMD_CLASS(N) \
	class N : public ADC::ClientCommandParser { \
		public: \
			N(ADC::CommandProxy* proxy, ADC::ClientSession* session) : ADC::ClientCommandParser(proxy, session) { }; \
			bool invoke(ADC::Command*) override; \
	};


	ADC_HUB_CMD_CLASS(CmdGetPass);
	ADC_HUB_CMD_CLASS(CmdSupport);
	ADC_HUB_CMD_CLASS(CmdSID);
	ADC_HUB_CMD_CLASS(CmdInfo);
	ADC_HUB_CMD_CLASS(CmdStatus);
	ADC_HUB_CMD_CLASS(CmdQuit);
	ADC_HUB_CMD_CLASS(CmdMessage);
	ADC_HUB_CMD_CLASS(CmdSearch);
	ADC_HUB_CMD_CLASS(CmdResult);
	ADC_HUB_CMD_CLASS(CmdPartialStatus);
	ADC_HUB_CMD_CLASS(CmdCTM);
	ADC_HUB_CMD_CLASS(CmdRCTM);
	ADC_HUB_CMD_CLASS(CmdNAT);
	ADC_HUB_CMD_CLASS(CmdRNT);

	/*
	 * The two bulletin board commands a hub sends: BBD describing a board, BBL
	 * carrying one entry of one. There is no third - BBP travels the other way, and
	 * a hub that sent one would be talking to itself.
	 */
	ADC_HUB_CMD_CLASS(CmdBoardDesc);
	ADC_HUB_CMD_CLASS(CmdBoardEntry);

	ADC_CLIENT_CMD_CLASS(CCmdSUP);
	ADC_CLIENT_CMD_CLASS(CCmdINF);
	ADC_CLIENT_CMD_CLASS(CCmdSTA);
	ADC_CLIENT_CMD_CLASS(CCmdGET);
	ADC_CLIENT_CMD_CLASS(CCmdSND);
	ADC_CLIENT_CMD_CLASS(CCmdGFI);
	ADC_CLIENT_CMD_CLASS(CCmdRES);
	
}

#endif // HAVE_QUICKDC_ADCCOMMANDS_H
