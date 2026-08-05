/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "share/searchrequest.h"
#include "share/searchresult.h"
#include "network/adc/adccommands.h"
#include "network/adc/cid.h"
#include "network/keyprint.h"
#include <string_view>

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
	session->onSTA(cmd->getArgument(0), cmd->getArgument(1));
	return true;
}

bool ADC::CCmdGET::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	QDBG("ADC::CCmdGET::invoke");
	char* ttyp = cmd->getArgument(0);
	char* file = cmd->getArgument(1);
	char* toff = cmd->getArgument(2);
	char* tlen = cmd->getArgument(3);
	/* getArgument() strips the name, so the flag is at [0]. */
	char* t_zl = cmd->getArgument("ZL", 2, 4);

	/*
	 * A peer can send this with anything after it, or nothing at all, and it
	 * arrives before any of it has been authorised. getArgument() answers null
	 * past the end, and the type is read four characters deep.
	 */
	if (!ttyp || strlen(ttyp) != 4 || !file || !toff || !tlen) return false;

	uint32_t type = FOURCC(static_cast<uint32_t>(ttyp[0]), static_cast<uint32_t>(ttyp[1]),
	                       static_cast<uint32_t>(ttyp[2]), static_cast<uint32_t>(ttyp[3]));
	uint64_t offset = Samurai::Util::Convert::to_uint64(toff);
	int64_t len = Samurai::Util::Convert::to_int64(tlen);
	bool zl = (t_zl && t_zl[0] == '1');

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
	/* getArgument() strips the name, so the flag is at [0]. */
	char* t_zl = cmd->getArgument("ZL", 2, 4);

	/*
	 * A peer can send this with anything after it, or nothing at all, and it
	 * arrives before any of it has been authorised. getArgument() answers null
	 * past the end, and the type is read four characters deep.
	 */
	if (!ttyp || strlen(ttyp) != 4 || !file || !toff || !tlen) return false;

	uint32_t type = FOURCC(static_cast<uint32_t>(ttyp[0]), static_cast<uint32_t>(ttyp[1]),
	                       static_cast<uint32_t>(ttyp[2]), static_cast<uint32_t>(ttyp[3]));
	uint64_t offset = Samurai::Util::Convert::to_uint64(toff);
	int64_t len = Samurai::Util::Convert::to_int64(tlen);
	bool zl = (t_zl && t_zl[0] == '1');

	session->onSND(file, type, offset, len, zl);
	return true;
}

bool ADC::CCmdGFI::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::High);
	QDBG("ADC::CCmdGFI::invoke");
	char* ttyp = cmd->getArgument(0);
	char* file = cmd->getArgument(1);

	/*
	 * A peer can send this with anything after it, or nothing at all, and it
	 * arrives before any of it has been authorised. getArgument() answers null
	 * past the end, and the type is read four characters deep.
	 */
	if (!ttyp || strlen(ttyp) != 4 || !file) return false;

	uint32_t type = FOURCC(static_cast<uint32_t>(ttyp[0]), static_cast<uint32_t>(ttyp[1]),
	                       static_cast<uint32_t>(ttyp[2]), static_cast<uint32_t>(ttyp[3]));
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

	/* What the hub answered decides what may be sent, so the session is told once the
	   whole list has been read. */
	session->onSupports();
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
		if (extract_int_tmp__) var = Samurai::Util::Convert::to_int32(extract_int_tmp__); \
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

		/* The user manager owns the user and hands out a const pointer; the
		   fields below are updated in place. */
		QuickDC::User* u = const_cast<QuickDC::User*>(session->users->getUser(id));
		if (!u) {
			u = new QuickDC::User(id,
				cmd->getArgument("NI", 2),
				cmd->getArgument("DE", 2),
				cmd->getArgument("EM", 2),
				0);
			changed = true;

			/*
			 * Unlike CCmdINF above, nothing has checked this one: a broadcast
			 * INF can carry any ID it likes, or none at all. Storing a
			 * malformed CID would leave us matching queue sources against
			 * something that is not a client ID.
			 */
			if (ADC::CID::verify(cid))
				u->setClientID(reinterpret_cast<uint8_t*>(cid));
		}

		uint64_t oldSize = u->getSharedBytes();
		uint64_t shared = 0;
		tmp = cmd->getArgument("SS", 2);
		if (tmp) shared = Samurai::Util::Convert::to_int64(tmp);
			
		uint32_t numslots = 0;
		EXTRACT_INT(numslots, "SL");
		
		int hub_cnt_op = 0;
		EXTRACT_INT(hub_cnt_op, "HO");

		int hub_cnt_reg = 0;
		EXTRACT_INT(hub_cnt_reg, "HR");
		
		int hub_cnt_user = 0;
		EXTRACT_INT(hub_cnt_user, "HN");
		
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

		/*
		 * KEYP: the keyprint the user says their certificate has. Unlike the
		 * fields above this one is only touched when the field is actually
		 * present - an INF update carries only what changed, and quietly
		 * forgetting a keyprint would turn a later peer connection back into
		 * an unverified one without anyone noticing.
		 *
		 * A keyprint we cannot read replaces the one we had, rather than
		 * leaving a stale value behind to raise a false alarm against.
		 */
		if (cmd->haveArgument("KP", 2))
		{
			char* kp = cmd->getArgument("KP", 2);

			if (kp && !QuickDC::Keyprint::isValid(kp))
			{
				QDBG("Ignoring the unusable keyprint '%s'", kp);
				kp = 0;
			}

			if (u->setKeyprint(kp)) changed = true;
		}

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

	/* getArgument() strips the name, so a flag's value is at [0]. */
	char* t_act = cmd->getArgument("ME", 2, 1);
	bool action = (t_act && t_act[0] == '1');

	char* t_rich = cmd->getArgument("RT", 2, 1);
	bool richText = (t_rich && t_rich[0] == '1');

	char* privat = cmd->getArgument("PM", 2, 1);
	char* msg = cmd->getArgument(0);
	
	
	if (cmd->isCommandBroadcast() || cmd->isCommandFeature()) {
		
		/* broadcast message */
		session->onPublicChat(cmd->getSourceID(), msg, action, richText);
		return true;
		
	} else if (cmd->isCommandHubInfo()) {
		
		/* hub info message */
		session->onPublicChat(0, cmd->getArgument(0), action, richText);
		return true;
		
	} else if (cmd->isCommandDirect() || cmd->isCommandEcho()) {
		/* direct message */
		if (privat) {
			session->onPrivateChat(cmd->getTargetID(), cmd->getSourceID(), msg, privat, action, richText);
		} else {
			session->onPublicChat(cmd->getSourceID(), msg, action, richText);
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
		size = Samurai::Util::Convert::to_uint64(szeq);
	} else if (szge) {
		size_type = QuickDC::Share::SearchRequest::SizeMin;
		size = Samurai::Util::Convert::to_uint64(szge);
	} else if (szle) {
		size_type = QuickDC::Share::SearchRequest::SizeMax;
		size = Samurai::Util::Convert::to_uint64(szle);
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

	/* The argument names mirror what HubSession::EventSearchReply() sends. */
	char* filename = cmd->getArgument("FN", 2, 0);
	char* s_size   = cmd->getArgument("SI", 2, 0);
	char* tth      = cmd->getArgument("TR", 2, 0);
	char* s_depth  = cmd->getArgument("TD", 2, 0);
	char* token    = cmd->getArgument("TO", 2, 0);
	char* s_slots  = cmd->getArgument("SL", 2, 0);

	if (!filename || !*filename)
	{
		QDBG("Ignoring a search result with no filename");
		return false;
	}

	const char* source = cmd->getSourceID();
	if (!source)
	{
		QDBG("Ignoring a search result from nobody");
		return false;
	}

	uint64_t size = (s_size) ? Samurai::Util::Convert::to_uint64(s_size) : 0;
	size_t depth  = (s_depth) ? static_cast<size_t>(Samurai::Util::Convert::to_int32(s_depth)) : 0;
	size_t slots  = (s_slots) ? static_cast<size_t>(Samurai::Util::Convert::to_int32(s_slots)) : 0;

	QuickDC::Share::SearchResult* result =
		new QuickDC::Share::SearchResult(filename, size, slots, tth, depth, token);

	/*
	 * The user manager owns the user; the search result only borrows it. The
	 * cast is because getUser() hands out a const pointer while SearchResult
	 * stores a mutable one.
	 */
	result->setUser(const_cast<QuickDC::User*>(session->getUser(source)));

	/* Takes ownership of 'result'. */
	session->onSearchResult(result);
	return true;
}

/*
 * The grammar in ADC 0.12 flags the token "TO", but the DC++ family sends it as
 * a plain positional field and reads it back the same way, so accept either.
 */
static char* adc_connect_token(ADC::Command* cmd, size_t position) {
	char* token = cmd->getArgument(position);
	if (token && std::string_view(token).starts_with("TO")) return &token[2];
	if (token) return token;
	return cmd->getArgument("TO", 2, position);
}

bool ADC::CmdPartialStatus::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Low);

	char* tth = cmd->getArgument("TR", 2, 0);
	char* pi  = cmd->getArgument("PI", 2, 0);
	if (!tth || !pi) return false;

	/* Hub routed, so the sender is named by SID; sources are keyed on CID. */
	const QuickDC::User* user = session->getUser(cmd->getSourceID());
	if (!user) return false;

	session->onPartialStatus(reinterpret_cast<const char*>(user->getClientID()), tth, pi);
	return true;
}

bool ADC::CmdCTM::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	char* proto = cmd->getArgument(0);
	int port    = Samurai::Util::Convert::to_int32(cmd->getArgument(1));

	char* token = adc_connect_token(cmd, 2);

	if (port > 0 && port < 65536) {
		session->onActiveConnect(cmd->getSourceID(), proto, port, token);
		return true;
	}
	return false;
}

bool ADC::CmdRCTM::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	char* proto = cmd->getArgument(0);
	char* token = adc_connect_token(cmd, 1);
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
	command.addArgument("AD", ADC_EXT_PFSR);

	command.addArgument("AD", ADC_EXT_ADCS);
	
	if (clientMode)
	{
		command.addArgument("AD", ADC_EXT_BZIP);
		command.addArgument("AD", ADC_EXT_ZLIG);
	}
	else
	{
		command.addArgument("AD", ADC_EXT_PING);
		command.addArgument("AD", ADC_EXT_BBS);

		/*
		 * Offered by a client and answered by a hub, both of which come through here.
		 * A peer to peer session has no use for it: a message carrying the RT flag
		 * only ever travels through a hub.
		 */
		command.addArgument("AD", ADC_EXT_RICHTEXT);
	}
}
