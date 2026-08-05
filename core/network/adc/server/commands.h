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

namespace LocalHub {

class Hub;

class Handler : public ADC::CommandParser
{
	public:
		Handler(CommandProxy* proxy, Hub* hub);
		virtual ~Handler();

		Handler(const Handler&) = delete;
		Handler& operator=(const Handler&) = delete;
		
		virtual bool invoke(Command*) override = 0;
		
	protected:
		Hub* hub;
		CommandProxy* proxy;
};

#define ADC_LOCALHUB_CMD_CLASS(N) \
	class N : public ADC::LocalHub::Handler { \
		public: \
			N(ADC::CommandProxy* proxy, ADC::LocalHub::Hub* hub) : ADC::LocalHub::Handler(proxy, hub) { }; \
			bool invoke(ADC::Command*) override; \
	};

	ADC_LOCALHUB_CMD_CLASS(CmdSUP);
	ADC_LOCALHUB_CMD_CLASS(CmdPAS);
	ADC_LOCALHUB_CMD_CLASS(CmdINF);
	ADC_LOCALHUB_CMD_CLASS(CmdMSG);
	ADC_LOCALHUB_CMD_CLASS(CmdSCH);
	ADC_LOCALHUB_CMD_CLASS(CmdRES);
	ADC_LOCALHUB_CMD_CLASS(CmdCTM);
	ADC_LOCALHUB_CMD_CLASS(CmdRCM);
	ADC_LOCALHUB_CMD_CLASS(CmdNAT);
	ADC_LOCALHUB_CMD_CLASS(CmdRNT);
	ADC_LOCALHUB_CMD_CLASS(CmdSTA);
	ADC_LOCALHUB_CMD_CLASS(CmdDSC);

} // namespace LocalHub
} // namespace ADC

#endif // HAVE_QUICKDC_ADC_LOCALHUB_COMMANDS_H
