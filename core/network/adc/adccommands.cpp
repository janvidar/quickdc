/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "share/searchrequest.h"
#include "network/adc/adccommands.h"
#include "network/adc/cid.h"

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/

bool ADC::CCmdSUP::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	for (size_t n = 0; cmd->getArgument(n); n++) {
		char* arg = cmd->getArgument(n);
		if (arg && strlen(arg) == 6)
			if (arg[0] == 'A' && arg[1] == 'D')
				session->addSupport(&arg[2]);
	}
	
	session->onSUP();
	return true;
}

bool ADC::CCmdINF::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	char* cid = cmd->getArgument("ID", 2);
	char* tok = cmd->getArgument("TO", 2);
	
	if (ADC::CID::verify(cid)) {
		session->onINF(cid, tok);
		return true;
	}
	return false;
}

bool ADC::CCmdSTA::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	QDBG("ADC::CCmd:STA:invoke");
	return false;
}

bool ADC::CCmdGET::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	QDBG("ADC::CCmdGET::invoke");
	char* ttyp = cmd->getArgument(0);
	char* file = cmd->getArgument(1);
	char* toff = cmd->getArgument(2);
	char* tlen = cmd->getArgument(3);
	char* t_zl = cmd->getArgument("ZL", 2, 4);
	uint32_t type = FOURCC(ttyp[0], ttyp[1], ttyp[2], ttyp[3]);
	uint64_t offset = quickdc_atoull(toff);
	int64_t len = quickdc_atoll(tlen);
	bool zl = (t_zl && t_zl[1] == '1');

	session->onGET(file, type, offset, len, zl);
	return true;
}

bool ADC::CCmdSND::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	QDBG("ADC::CCmdSND::invoke");
	char* ttyp = cmd->getArgument(0);
	char* file = cmd->getArgument(1);
	char* toff = cmd->getArgument(2);
	char* tlen = cmd->getArgument(3);
	char* t_zl = cmd->getArgument("ZL1", 3, 4);
	uint32_t type = FOURCC(ttyp[0], ttyp[1], ttyp[2], ttyp[3]);
	uint64_t offset = quickdc_atoull(toff);
	int64_t len = quickdc_atoll(tlen);

	session->onSND(file, type, offset, len, t_zl);
	return true;
}

bool ADC::CCmdGFI::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	QDBG("ADC::CCmdGFI::invoke");
	char* ttyp = cmd->getArgument(0);
	char* file = cmd->getArgument(1);
	uint32_t type = FOURCC(ttyp[0], ttyp[1], ttyp[2], ttyp[3]);
	session->onGFI(file, type);
	return true;
}

bool ADC::CCmdRES::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	QDBG("ADC::CCmdRES::invoke");
	return false;
}


/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/

bool ADC::CmdSID::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	char* sid = cmd->getArgument(0);
	if (!sid) return false;
	session->onSID(sid);
	return true;
}

bool ADC::CmdSupport::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	for (size_t n = 0; cmd->getArgument(n); n++) {
		char* arg = cmd->getArgument(n);
		if (arg && strlen(arg) == 6)
			if (arg[0] == 'A' && arg[1] == 'D')
				session->addSupport(&arg[2]);
	}
	return true;
}

bool ADC::CmdStatus::invoke(ADC::Command* cmd) {
	if (cmd->isCommandHubInfo())
		cmd->setPriority(ADC::Command::High);
	else
		cmd->setPriority(ADC::Command::Normal);
	session->onStatus(cmd);
	return true;
}



bool ADC::CmdGetPass::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	session->onRequestPasword(cmd->getArgument(0));
	return true;
}

#define EXTRACT_INT(var, prefix) \
	if (cmd->haveArgument(prefix, 2)) { \
		var = 0; \
		char* extract_int_tmp__ = cmd->getArgument(prefix, 2); \
		if (extract_int_tmp__) var = quickdc_atoi(extract_int_tmp__); \
	}

bool ADC::CmdInfo::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	char* tmp = 0;
	
	if (cmd->getFourcc() == FOURCC('I','I','N','F')) { /* hub information */
		/* cmd->getArgument("HU", 2) */

		tmp = cmd->getArgument("NI", 2);
		if (tmp) session->onHubName(tmp);
		
		tmp = cmd->getArgument("DE", 2);
		if (tmp) session->onHubDesc(tmp);
		
		tmp = cmd->getArgument("VE", 2);
		if (tmp) session->onHubVersion(tmp);
	} else {
		bool changed = false;

		char* id = cmd->getSourceID();
		char* cid = cmd->getArgument("ID", 2);

		QuickDC::User* u = (QuickDC::User*) session->users->getUser(id);
		if (!u) {
			u = new QuickDC::User(id,
				cmd->getArgument("NI", 2),
				cmd->getArgument("DE", 2),
				cmd->getArgument("EM", 2),
				0);
			changed = true;
			u->setClientID((uint8_t*) cid);
		}

		uint64_t oldSize = u->getSharedBytes();
		uint64_t shared = 0;
		tmp = cmd->getArgument("SS", 2);
		if (tmp) shared = quickdc_atoll(tmp);
			
		uint32_t numslots = 0;
		EXTRACT_INT(numslots, "SL");
		
		int hub_cnt_op = 0;
		EXTRACT_INT(hub_cnt_op, "HO");

		int hub_cnt_reg = 0;
		EXTRACT_INT(hub_cnt_reg, "HR");
		
		int hub_cnt_user = 0;
		EXTRACT_INT(hub_cnt_reg, "HN");
		
		int op = 0;
		EXTRACT_INT(op, "OP"); // Operator -- FIXME - Use CT instead!
		if (op)
			if (u->setOp()) changed = true;
		
		int port_ipv4 = 0;
		EXTRACT_INT(port_ipv4, "U4");
		int port_ipv6 = 0;
		EXTRACT_INT(port_ipv6, "U6");
		
		if (u->setDescription(cmd->getArgument("DE", 2))) changed = true;
		if (u->setEmail(cmd->getArgument("EM", 2))) changed = true;
		if (u->setSharedBytes(shared)) changed = true;
		if (u->setUserAgent(cmd->getArgument("VE", 2))) changed = true;
		if (u->setNumSlots(numslots)) changed = true;
		if (u->setHubsOperator(hub_cnt_op)) changed = true;
		if (u->setHubsRegistered(hub_cnt_reg)) changed = true;
		if (u->setHubsRegular(hub_cnt_user)) changed = true;
		
		if (u->setAddressIPv4(cmd->getArgument("I4", 2))) changed = true;
		if (u->setAddressIPv6(cmd->getArgument("I6", 2))) changed = true;
		if (u->setPortIPv4(port_ipv4)) changed = true;
		if (u->setPortIPv6(port_ipv6)) changed = true;
		
		// TODO: Add active/passive mode.
		session->onUserInfo(id, u, oldSize, changed);
	}
	
	// for (size_t n = 0; cmd->getArgument(n); n++)
	//	QDBG(" --- param: '%s'", cmd->getArgument(n));
	return true;
}

bool ADC::CmdQuit::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	char* id  = cmd->getArgument(0);
	char* msg = cmd->getArgument("MS", 2);
	// bool disconnect = cmd->hasArgument("DI", 2); -- FIXME
	session->users->quit(id, msg);
	return true;
}

bool ADC::CmdMessage::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	char* t_act = cmd->getArgument("ME", 2, 1);
	bool action = (t_act && t_act[1] == '1');
	char* privat = cmd->getArgument("PM", 2, 1);
	char* msg = cmd->getArgument(0);
	
	
	if (cmd->isCommandBroadcast() || cmd->isCommandFeature()) {
		
		/* broadcast message */
		session->onPublicChat(cmd->getSourceID(), msg, action);
		return true;
		
	} else if (cmd->isCommandHubInfo()) {
		
		/* hub info message */
		session->onPublicChat(0, cmd->getArgument(0), action);
		return true;
		
	} else if (cmd->isCommandDirect() || cmd->isCommandEcho()) {
		/* direct message */
		if (privat) {
			session->onPrivateChat(cmd->getTargetID(), cmd->getSourceID(), msg, privat, action);
		} else {
			session->onPublicChat(cmd->getSourceID(), msg, action);
		}
		return true;
	}
	return false;
}

bool ADC::CmdSearch::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Low);
	if (!cmd->getSourceID()) return false;
	const QuickDC::User* user = session->users->getUser(cmd->getSourceID());
	if (!user) return false;
	
	QuickDC::Share::SearchRequest* request = new QuickDC::Share::SearchRequest(session);
	for (size_t n = 0; cmd->getArgument(n); n++) {
		char* arg = cmd->getArgument(n);
		if (arg && (strlen(arg) > 2))
		{
			if (arg[0] == 'A' && arg[1] == 'N')
				request->addInclusion(&arg[2]);
			else if (arg[0] == 'N' && arg[1] == 'O')
				request->addExclusion(&arg[2]);
			else if (arg[0] == 'E' && arg[1] == 'X')
				request->addExtension(&arg[2]);
		}
	}
	
	char* szle = cmd->getArgument("LE", 2);
	char* szge = cmd->getArgument("GE", 2);
	char* szeq = cmd->getArgument("EQ", 2);
	
	QuickDC::Share::SearchRequest::SizePolicy size_type = QuickDC::Share::SearchRequest::SizeAny;
	uint64_t size = 0;
	if (szeq) {
		size_type = QuickDC::Share::SearchRequest::SizeExact;
		size = quickdc_atoull(szeq);
	} else if (szge) {
		size_type = QuickDC::Share::SearchRequest::SizeMin;
		size = quickdc_atoull(szge);
	} else if (szle) {
		size_type = QuickDC::Share::SearchRequest::SizeMax;
		size = quickdc_atoull(szle);
	}
	
	char* token = cmd->getArgument("TO", 2);
	char* tth   = cmd->getArgument("TR", 2);
	char* type  = cmd->getArgument("TY", 2);
	
	QuickDC::Share::SearchRequest::FilePolicy file_type = QuickDC::Share::SearchRequest::SearchAll;
	if (type) {
		if      (type[0] == '1') file_type = QuickDC::Share::SearchRequest::SearchFiles;
		else if (type[0] == '2') file_type = QuickDC::Share::SearchRequest::SearchDirectories;
	}
	
	request->setTTH(tth);
	request->setType(file_type);
	request->setToken(token);
	request->setSizePolicy(size_type);
	request->setSize(size);
	request->setUser(user);
	
	session->onSearch(request);
	
	delete request;
	return true;
}

bool ADC::CmdResult::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Low);
	QDBG("ADC Command: RES");
	return false;
}

bool ADC::CmdCTM::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	char* proto = cmd->getArgument(0);
	int port    = quickdc_atoi(cmd->getArgument(1));
	
	// char* token = cmd->getArgument("TO", 2, 2); // FIXME: DC++ error, not sending TO prefix ???
	char* token = cmd->getArgument(2);
	
	if (port > 0 && port < 65536) {
		session->onActiveConnect(cmd->getSourceID(), proto, port, token);
		return true;
	}
	return false;
}

bool ADC::CmdRCTM::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	char* proto = cmd->getArgument(0);
	char* token = cmd->getArgument("TO", 2, 1);
	session->onPassiveConnect(cmd->getSourceID(), proto, token);
	return true;
}

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/

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

// BBS extension
bool ADC::HubCmdMHE::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onMHE(cmd);
	return true;
}

bool ADC::HubCmdMBO::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	hub->onMBO(cmd);
	return true;
}

void ADC::addSupportFlags(ADC::Command& command, bool clientMode)
{
	command.addArgument("AD", ADC_BASE);
	command.addArgument("AD", ADC_BASE_COMPAT); // TODO: Remove when ldcpp does not require it.
	command.addArgument("AD", ADC_HASH_TIGER);

#ifdef SSL_SUPPORT
	command.addArgument("AD", ADC_EXT_ADCS);
#endif
	
	if (clientMode)
	{
		command.addArgument("AD", ADC_EXT_BZIP);
		command.addArgument("AD", ADC_EXT_ZLIG);
	}
	else
	{
		command.addArgument("AD", ADC_EXT_PING);
		command.addArgument("AD", ADC_EXT_BBS);
	}
}
