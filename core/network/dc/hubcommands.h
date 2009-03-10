/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCHUBCOMMANDS_H
#define HAVE_QUICKDC_DCHUBCOMMANDS_H

#include "network/dc/hubsession.h"
#include "network/dc/dccommandparser.h"

namespace DC {

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
	DC_HCMD_CLASS(Supports,       "$Supports");
	DC_HCMD_CLASS(To,             "$To:");
	DC_HCMD_CLASS(UserCommand,    "$UserCommand");
	DC_HCMD_CLASS(UserIP,         "$UserIP");
	DC_HCMD_CLASS(ValidateDenied, "$ValidateDenide"); /* protocol typo */

}
#undef DC_HCMD_CLASS

#endif // HAVE_QUICKDC_DCHUBCOMMANDS_H
