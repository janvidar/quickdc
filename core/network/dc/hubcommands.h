/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCHUBCOMMANDS_H
#define HAVE_QUICKDC_DCHUBCOMMANDS_H

#include "network/dc/hubsession.h"
#include "network/dc/dccommandparser.h"
#include <string>

namespace DC {

/**
 * Decode a $SR line, a reply to a search we sent out, into its parts.
 *
 * Syntax:
 *   $SR {nick} {path}\x05{size} {free}/{total}\x05{hub name} ({host}:{port})
 *   $SR {nick} {path} {free}/{total}\x05{hub name} ({host}:{port})
 *
 * The first is a file, the second a directory - a directory has no size, so
 * the \x05 before it is simply absent. When the sender knows the TTH root, the
 * hub name is replaced by "TTH:{root}". A reply relayed through the hub, in
 * answer to a passive search, has "\x05{our nick}" appended.
 *
 * The path and the hub name may both contain spaces, and the number of \x05
 * separators varies, so the fields are found by locating the
 * "({host}:{port})" trailer and working outwards from it.
 *
 * Split out from SearchResultParser::invoke() so that the decoding can be
 * tested without a live session.
 *
 * @param size      zero for a directory result.
 * @param tth       empty unless the sender supplied a 39 character root.
 * @return false if the line is not a well formed $SR.
 */
bool parseSearchResult(const char* data, size_t length,
	std::string& nick, std::string& path, uint64_t& size,
	size_t& freeslots, std::string& tth);


class HubSession;
class CommandProxy;
class Command;
class Tag;

#define DC_HCMD_CLASS(name, cmd) \
	class name : public HubCommandParser \
	{ \
		public: \
			name(DC::CommandProxy* proxy, DC::HubSession* session) : \
				DC::HubCommandParser(proxy, session, cmd) { }; \
			bool invoke(const char* line, size_t length); \
	}
	
	DC_HCMD_CLASS(BadPass,        "$BadPass");
	DC_HCMD_CLASS(ConnectToMe,    "$ConnectToMe");
	DC_HCMD_CLASS(ForceMove,      "$ForceMove");
	DC_HCMD_CLASS(GetPass,        "$GetPass");
	DC_HCMD_CLASS(Hello,          "$Hello");
	DC_HCMD_CLASS(HubIsFull,      "$HubIsFull");
	DC_HCMD_CLASS(HubName,        "$HubName");
	DC_HCMD_CLASS(LockParser,     "$Lock");
	DC_HCMD_CLASS(LoggedIn,       "$LogedIn"); /* protocol typo */
	DC_HCMD_CLASS(MyINFO,         "$MyINFO");
	DC_HCMD_CLASS(NickList,       "$NickList");
	DC_HCMD_CLASS(OpList,         "$OpList");
	DC_HCMD_CLASS(PublicChat,     "PublicChat"); /* special case */
	DC_HCMD_CLASS(ActionChat,     "ActionChat"); /* special case */
	DC_HCMD_CLASS(Quit,           "$Quit");
	DC_HCMD_CLASS(RevConnectToMe, "$RevConnectToMe");
	DC_HCMD_CLASS(Search,         "$Search");
	/* Named ...Parser to stay clear of QuickDC::Share::SearchResult. */
	DC_HCMD_CLASS(SearchResultParser, "$SR");
	DC_HCMD_CLASS(Supports,       "$Supports");
	DC_HCMD_CLASS(To,             "$To:");
	DC_HCMD_CLASS(UserCommand,    "$UserCommand");
	DC_HCMD_CLASS(UserIP,         "$UserIP");
	DC_HCMD_CLASS(ValidateDenied, "$ValidateDenide"); /* protocol typo */

}
#undef DC_HCMD_CLASS

#endif // HAVE_QUICKDC_DCHUBCOMMANDS_H
