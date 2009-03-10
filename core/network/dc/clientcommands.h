/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCCLIENTCOMMANDS_H
#define HAVE_QUICKDC_DCCLIENTCOMMANDS_H

#include "network/dc/dccommandparser.h"
#include "clientsession.h"
#include "network/dc/dccommandproxy.h"

#define DC_CCMD_CLASS(name, cmd) \
	class name : public ClientCommandParser \
	{ \
		public: \
			name(DC::CommandProxy* proxy, DC::ClientSession* session) : \
				DC::ClientCommandParser(proxy, session, cmd) { }; \
				\
				bool invoke(const char* line, size_t length); \
	};

namespace DC {

 /* NOTE: Remember to initialize this in DCConnection() */

DC_CCMD_CLASS(ClientMyNick,       "$MyNick")
DC_CCMD_CLASS(ClientLock,         "$Lock")
DC_CCMD_CLASS(ClientKey,          "$Key")
DC_CCMD_CLASS(ClientDirection,    "$Direction")
DC_CCMD_CLASS(ClientError,        "$Error")
DC_CCMD_CLASS(ClientFileLength,   "$FileLength")
DC_CCMD_CLASS(ClientMaxedOut,     "$MaxedOut")
DC_CCMD_CLASS(ClientGetListLen,   "$GetListLen")
DC_CCMD_CLASS(ClientGet,          "$Get")
DC_CCMD_CLASS(ClientSupports,     "$Supports")
DC_CCMD_CLASS(ClientSend,         "$Send")
DC_CCMD_CLASS(ClientSending,      "$Sending")
DC_CCMD_CLASS(ClientFailed,       "$Failed")
DC_CCMD_CLASS(ClientExtGetZBlock, "$GetZBlock")
DC_CCMD_CLASS(ClientExtGetUBlock, "$GetUBlock")
DC_CCMD_CLASS(ClientExtADCGET,    "$ADCGET");
DC_CCMD_CLASS(ClientExtADCSND,    "$ADCSND");

}

#undef DC_CCMD_CLASS

#endif // HAVE_QUICKDC_DCHUBCOMMANDS_H
