/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCCOMMANDS_H
#define HAVE_QUICKDC_ADCCOMMANDS_H

#include "network/adc/adccommandparser.h"
#include "network/adc/client/adchubsession.h"
#include "network/adc/server/adchub.h"
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
			bool invoke(ADC::Command*); \
	};


#define ADC_CLIENT_CMD_CLASS(N) \
	class N : public ADC::ClientCommandParser { \
		public: \
			N(ADC::CommandProxy* proxy, ADC::ClientSession* session) : ADC::ClientCommandParser(proxy, session) { }; \
			bool invoke(ADC::Command*); \
	};


#define ADC_LOCALHUB_CMD_CLASS(N) \
	class N : public ADC::LocalHubParser { \
		public: \
			N(ADC::CommandProxy* proxy, ADC::Hub* hub) : ADC::LocalHubParser(proxy, hub) { }; \
			bool invoke(ADC::Command*); \
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
	ADC_HUB_CMD_CLASS(CmdCTM);
	ADC_HUB_CMD_CLASS(CmdRCTM);

	ADC_CLIENT_CMD_CLASS(CCmdSUP);
	ADC_CLIENT_CMD_CLASS(CCmdINF);
	ADC_CLIENT_CMD_CLASS(CCmdSTA);
	ADC_CLIENT_CMD_CLASS(CCmdGET);
	ADC_CLIENT_CMD_CLASS(CCmdSND);
	ADC_CLIENT_CMD_CLASS(CCmdGFI);
	ADC_CLIENT_CMD_CLASS(CCmdRES);
	
	ADC_LOCALHUB_CMD_CLASS(HubCmdSUP);
	ADC_LOCALHUB_CMD_CLASS(HubCmdPAS);
	ADC_LOCALHUB_CMD_CLASS(HubCmdINF);
	ADC_LOCALHUB_CMD_CLASS(HubCmdMSG);
	ADC_LOCALHUB_CMD_CLASS(HubCmdSCH);
	ADC_LOCALHUB_CMD_CLASS(HubCmdRES);
	ADC_LOCALHUB_CMD_CLASS(HubCmdCTM);
	ADC_LOCALHUB_CMD_CLASS(HubCmdRCM);
	ADC_LOCALHUB_CMD_CLASS(HubCmdSTA);
	ADC_LOCALHUB_CMD_CLASS(HubCmdDSC);
	
	// BBS extension
	ADC_LOCALHUB_CMD_CLASS(HubCmdMHE);
	ADC_LOCALHUB_CMD_CLASS(HubCmdMBO);

}

#endif // HAVE_QUICKDC_ADCCOMMANDS_H
