/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "share/searchrequest.h"
#include "share/searchresult.h"
#include "network/adc/adcbbs.h"
#include "network/adc/adccommands.h"
#include "network/adc/cid.h"
#include "network/keyprint.h"

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
		if (!arg || strlen(arg) != 6) continue;

		if (arg[0] == 'A' && arg[1] == 'D')
			session->addSupport(&arg[2]);

		/* A hub is as free to take a feature back as to offer one, and a SUP that
		   says so arrives long after the login exchange - see removeSupport(). */
		else if (arg[0] == 'R' && arg[1] == 'M')
			session->removeSupport(&arg[2]);
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

/*
 * A field is applied only when the INF actually carries it.
 *
 * An INF update names what changed and nothing else - that is what a hub
 * relays, and what this client now sends - so a field that is absent is one
 * this user has not touched, not one they have cleared. Applying it regardless
 * would empty a description on every unrelated share size change, and empty the
 * 'SU' the connection strategy reads, leaving a peer that supports TCP4 and
 * NATT looking like it supports neither.
 *
 * Absent and empty are told apart by haveArgument(): getArgument() answers the
 * empty string for a field that is there and blank, and null for one that is
 * not there at all.
 */
#define INF_APPLY_STRING(prefix, setter) \
	if (cmd->haveArgument(prefix, 2)) { \
		if (u->setter(cmd->getArgument(prefix, 2))) changed = true; \
	}

#define INF_APPLY_INT(prefix, setter, var) \
	if (cmd->haveArgument(prefix, 2)) { \
		if (u->setter(var)) changed = true; \
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

		uint32_t sharedFiles = 0;
		EXTRACT_INT(sharedFiles, "SF");
		
		int hub_cnt_op = 0;
		EXTRACT_INT(hub_cnt_op, "HO");

		int hub_cnt_reg = 0;
		EXTRACT_INT(hub_cnt_reg, "HR");
		
		int hub_cnt_user = 0;
		EXTRACT_INT(hub_cnt_user, "HN");
		
		/*
		 * CT: what the hub says the user is, as a set of bits - a bot, someone
		 * registered, an operator, and so on. Only the operator bit is read. A
		 * super user and a hub owner are required to set it alongside their own,
		 * so it alone answers the one question a user list asks.
		 *
		 * Only applied when the field is there, for the reason given for AW below:
		 * an INF updating a user carries the fields that changed and no others, so
		 * an absent CT would otherwise read as a demotion.
		 */
		if (cmd->haveArgument("CT", 2))
		{
			constexpr int CT_OPERATOR = 0x04;

			const char* type = cmd->getArgument("CT", 2);
			const int flags = type ? Samurai::Util::Convert::to_int32(type) : 0;

			if (u->setOp((flags & CT_OPERATOR) != 0)) changed = true;
		}

		/*
		 * AW: 1 is away, 2 is away and not reading the chat. Both are away here -
		 * the difference is a hint to the hub about which broadcasts to bother
		 * sending, not a third state anybody looking at a user list wants.
		 *
		 * Only applied when the field is actually there. An INF that updates a user
		 * carries the fields that changed and no others, so an absent AW means
		 * nothing new was said about it rather than that they are back - and a
		 * client coming back sends the field empty, which reads as not away.
		 */
		if (cmd->haveArgument("AW", 2))
		{
			const char* value = cmd->getArgument("AW", 2);
			const bool away = value && Samurai::Util::Convert::to_int32(value) != 0;

			if (u->setAway(away)) changed = true;
		}
		
		int port_ipv4 = 0;
		EXTRACT_INT(port_ipv4, "U4");
		int port_ipv6 = 0;
		EXTRACT_INT(port_ipv6, "U6");
		
		INF_APPLY_STRING("DE", setDescription);
		INF_APPLY_STRING("EM", setEmail);
		INF_APPLY_INT("SS", setSharedBytes, shared);
		INF_APPLY_INT("SF", setSharedFiles, sharedFiles);
		INF_APPLY_STRING("VE", setUserAgent);
		INF_APPLY_INT("SL", setNumSlots, numslots);
		INF_APPLY_INT("HO", setHubsOperator, hub_cnt_op);
		INF_APPLY_INT("HR", setHubsRegistered, hub_cnt_reg);
		INF_APPLY_INT("HN", setHubsRegular, hub_cnt_user);

		INF_APPLY_STRING("SU", setSupports);
		INF_APPLY_STRING("I4", setAddressIPv4);
		INF_APPLY_STRING("I6", setAddressIPv6);
		INF_APPLY_INT("U4", setPortIPv4, port_ipv4);
		INF_APPLY_INT("U6", setPortIPv6, port_ipv6);

		/*
		 * KEYP: the keyprint the user says their certificate has. Read by hand
		 * rather than through the macros above, because a keyprint we cannot
		 * make sense of replaces the one we had rather than being ignored -
		 * leaving a stale value behind is what a false alarm would be raised
		 * against.
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

	/*
	 * TS: when the hub says the message was written, in seconds since the epoch.
	 * A hub replaying its history stamps each line with the time it was
	 * originally said, which is the whole point of the field - without it a
	 * replay would read as everything having been said at once, on login.
	 *
	 * A hub that sends none, or one this cannot make sense of, leaves the moment
	 * it arrived here. Every message carries a stamp either way, so what was
	 * replayed can be lined up against what was written down before.
	 */
	uint64_t timestamp = 0;

	if (cmd->haveArgument("TS", 2))
	{
		const char* when = cmd->getArgument("TS", 2);
		if (when) timestamp = Samurai::Util::Convert::to_uint64(when);
	}

	if (!timestamp) timestamp = static_cast<uint64_t>(time(0));

	if (cmd->isCommandBroadcast() || cmd->isCommandFeature()) {
		
		/* broadcast message */
		session->onPublicChat(cmd->getSourceID(), msg, action, richText, timestamp);
		return true;
		
	} else if (cmd->isCommandHubInfo()) {
		
		/* hub info message */
		session->onPublicChat(0, cmd->getArgument(0), action, richText, timestamp);
		return true;
		
	} else if (cmd->isCommandDirect() || cmd->isCommandEcho()) {
		/* direct message */
		if (privat) {
			session->onPrivateChat(cmd->getTargetID(), cmd->getSourceID(), msg, privat, action, richText, timestamp);
		} else {
			session->onPublicChat(cmd->getSourceID(), msg, action, richText, timestamp);
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
 * Positional, as ADC 1.0 and the DC++ family have it, and taken whole. The 0.12
 * grammar flagged it "TO", but no flag can be told apart from a token that merely
 * starts with those letters - one in 1024 of our own - and a token read short is
 * one the peer quotes back and nobody recognises.
 */
static char* adc_connect_token(ADC::Command* cmd, size_t position) {
	return cmd->getArgument(position);
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

/*
 * NATT carries the same three things a CTM does, in the same order: what to speak,
 * which port to dial, and which job it is for. The difference is whose port it is -
 * a NAT mapping the naming client made by talking to the hub, which only stays open
 * while both ends dial at once.
 */
bool ADC::CmdNAT::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	char* proto = cmd->getArgument(0);
	int port    = Samurai::Util::Convert::to_int32(cmd->getArgument(1));

	char* token = adc_connect_token(cmd, 2);

	if (port > 0 && port < 65536) {
		session->onNatConnect(cmd->getSourceID(), proto, port, token);
		return true;
	}
	return false;
}

bool ADC::CmdRNT::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);
	char* proto = cmd->getArgument(0);
	int port    = Samurai::Util::Convert::to_int32(cmd->getArgument(1));

	char* token = adc_connect_token(cmd, 2);

	if (port > 0 && port < 65536) {
		session->onNatReply(cmd->getSourceID(), proto, port, token);
		return true;
	}
	return false;
}

/*
 * Both of these are dropped on a hub that never announced BBS0, and dropped
 * silently: the extension requires it, and the reason is worth stating where the
 * check is. A descriptor is what puts a board in front of the user, so a hub that
 * has not agreed to the extension can otherwise announce boards to a client that
 * offered it and never heard back - and an entry is a claim about a post, which is
 * a claim nobody asked this hub for.
 *
 * Answered as handled rather than as a parse failure, since ignoring a command is
 * what was meant to happen: reporting it would put an unhandled command in the log
 * for a hub behaving exactly as it should.
 */
bool ADC::CmdBoardDesc::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);

	if (!session->supportsBulletins()) return true;

	QuickDC::Board board;
	if (!ADC::readBoard(cmd, board)) return false;

	session->onBoardDescription(board);
	return true;
}

bool ADC::CmdBoardEntry::invoke(ADC::Command* cmd) {
	cmd->setPriority(ADC::Command::Normal);

	if (!session->supportsBulletins()) return true;

	QuickDC::BoardEntry entry;
	if (!ADC::readEntry(cmd, entry)) return false;

	session->onBoardEntry(entry);
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
		/*
		 * Offered by a client and answered by a hub, both of which come through here.
		 * A peer to peer session has no use for it: a message carrying the RT flag
		 * only ever travels through a hub.
		 */
		command.addArgument("AD", ADC_EXT_RICHTEXT);
	}
}
