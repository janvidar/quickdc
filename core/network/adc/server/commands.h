/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_LOCALHUB_COMMANDS_H
#define HAVE_QUICKDC_ADC_LOCALHUB_COMMANDS_H

#include "network/adc/adccommandparser.h"
#include "network/adc/parser.h"
#include "network/adc/server/hub.h"

/*
 * The commands arriving at the hub we run, as opposed to the ones a remote hub
 * sends us.
 *
 * These live here rather than beside the client's handlers in adccommands.h for
 * two reasons. The names there said the opposite of what they meant -
 * ADC_HUB_CMD_CLASS generates the client's handlers, while the classes prefixed
 * HubCmd were the hub's - and adccommands.h had to include the hub server's
 * header to declare them, so every consumer of it saw the whole hub whether it
 * had any use for one or not.
 *
 * Each of these does the same small thing: set the priority the command travels
 * at, and hand it to the hub.
 */

namespace ADC {

class Command;
class CommandProxy;
class Hub;

class LocalHubParser : public ADC::CommandParser
{
	public:
		LocalHubParser(CommandProxy* proxy, Hub* hub);
		virtual ~LocalHubParser();

		LocalHubParser(const LocalHubParser&) = delete;
		LocalHubParser& operator=(const LocalHubParser&) = delete;
		
		virtual bool invoke(Command*) override = 0;
		
	protected:
		Hub* hub;
		CommandProxy* proxy;
};

#define ADC_LOCALHUB_CMD_CLASS(N) \
	class N : public ADC::LocalHubParser { \
		public: \
			N(ADC::CommandProxy* proxy, ADC::Hub* hub) : ADC::LocalHubParser(proxy, hub) { }; \
			bool invoke(ADC::Command*) override; \
	};

	ADC_LOCALHUB_CMD_CLASS(HubCmdSUP);
	ADC_LOCALHUB_CMD_CLASS(HubCmdPAS);
	ADC_LOCALHUB_CMD_CLASS(HubCmdINF);
	ADC_LOCALHUB_CMD_CLASS(HubCmdMSG);
	ADC_LOCALHUB_CMD_CLASS(HubCmdSCH);
	ADC_LOCALHUB_CMD_CLASS(HubCmdRES);
	ADC_LOCALHUB_CMD_CLASS(HubCmdCTM);
	ADC_LOCALHUB_CMD_CLASS(HubCmdRCM);
	ADC_LOCALHUB_CMD_CLASS(HubCmdNAT);
	ADC_LOCALHUB_CMD_CLASS(HubCmdRNT);
	ADC_LOCALHUB_CMD_CLASS(HubCmdSTA);
	ADC_LOCALHUB_CMD_CLASS(HubCmdDSC);

}

#endif // HAVE_QUICKDC_ADC_LOCALHUB_COMMANDS_H
