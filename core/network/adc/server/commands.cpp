/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include "network/adc/parser.h"
#include "network/adc/server/hub.h"
#include "network/adc/server/commands.h"

ADC::LocalHub::Handler::Handler(CommandProxy* proxy_, Hub* hub_)
	: hub(hub_)
	, proxy(proxy_)
{

}

ADC::LocalHub::Handler::~Handler()
{

}

bool ADC::LocalHub::Handler::invoke(ADC::Command*)
{
	QDBG("ADC::LocalHub::Handler::invoke(): Not invented here!");
	return false;
}

bool ADC::LocalHub::CmdSUP::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onSUP(cmd);
	return true;
}

bool ADC::LocalHub::CmdINF::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onINF(cmd);
	return true;
}

bool ADC::LocalHub::CmdMSG::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onMSG(cmd);
	return true;
}

bool ADC::LocalHub::CmdSCH::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Low);
	hub->onSCH(cmd);
	return true;
}

bool ADC::LocalHub::CmdRES::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Low);
	hub->onRES(cmd);
	return true;
}

bool ADC::LocalHub::CmdCTM::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onCTM(cmd);
	return true;
}

bool ADC::LocalHub::CmdRCM::invoke(ADC::Command* cmd) {
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
bool ADC::LocalHub::CmdNAT::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onNAT(cmd);
	return true;
}

bool ADC::LocalHub::CmdRNT::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onRNT(cmd);
	return true;
}

bool ADC::LocalHub::CmdPAS::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onPAS(cmd);
	return true;
}

bool ADC::LocalHub::CmdSTA::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onSTA(cmd);
	return true;
}

bool ADC::LocalHub::CmdDSC::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onDSC(cmd);
	return true;
}

/*
 * The two bulletin board commands a client sends. Neither is relayed - the hub
 * answers a BBL with index entries of its own and a BBP with the entry it made -
 * so the priority set here is what the hub's own reply inherits nothing from, and
 * Normal is what the work of reading a board is worth: less than the commands a
 * session needs to be on the hub at all, and more than a search.
 */
bool ADC::LocalHub::CmdBBL::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onBBL(cmd);
	return true;
}

bool ADC::LocalHub::CmdBBP::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	hub->onBBP(cmd);
	return true;
}
