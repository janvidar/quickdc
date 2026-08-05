/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include "network/adc/parser.h"
#include "network/adc/server/adchub.h"
#include "network/adc/server/commands.h"

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

bool ADC::HubCmdSUP::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onSUP(cmd);
	return true;
}

bool ADC::HubCmdINF::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onINF(cmd);
	return true;
}

bool ADC::HubCmdMSG::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onMSG(cmd);
	return true;
}

bool ADC::HubCmdSCH::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Low);
	hub->onSCH(cmd);
	return true;
}

bool ADC::HubCmdRES::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Low);
	hub->onRES(cmd);
	return true;
}

bool ADC::HubCmdCTM::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onCTM(cmd);
	return true;
}

bool ADC::HubCmdRCM::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onRCM(cmd);
	return true;
}

/*
 * NATT: two passive clients arranging to connect to each other. Relayed exactly
 * as CTM and RCM are - the hub is the only path between two clients that neither
 * can reach - and carrying a port that means something only to the client that
 * named it, so there is nothing here for the hub to check.
 */
bool ADC::HubCmdNAT::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onNAT(cmd);
	return true;
}

bool ADC::HubCmdRNT::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onRNT(cmd);
	return true;
}

bool ADC::HubCmdPAS::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onPAS(cmd);
	return true;
}

bool ADC::HubCmdSTA::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onSTA(cmd);
	return true;
}

bool ADC::HubCmdDSC::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onDSC(cmd);
	return true;
}
