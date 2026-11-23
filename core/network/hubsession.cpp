/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/hubsession.h"

#include "api/core.h"
#include "api/hub.h"
#include "chat/archive.h"
#include "network/user.h"

#include <time.h>

void QuickDC::HubSession::markEstablished()
{
	if (!established) established = time(0);
}

bool QuickDC::HubSession::wasEstablished() const
{
	return established != 0;
}

uint64_t QuickDC::HubSession::establishedFor() const
{
	if (!established) return 0;

	const time_t now = time(0);

	return now > established ? static_cast<uint64_t>(now - established) : 0;
}

bool QuickDC::HubSession::recordChat(const User* user, const char* hubName,
                                     const char* message, const char* conversation,
                                     bool action, bool richText, uint64_t timestamp)
{
	if (!message) return false;

	/*
	 * With nothing keeping a record, nothing can be a repeat of anything - so
	 * the answer is yes, show it. A core built without an archive still has
	 * working chat; it just does not remember it.
	 */
	if (!hub) return true;

	QuickDC::Core* core = &hub->getCore();
	if (!core->chat || !core->chat->isUsable()) return true;

	/* The address the session is on, which is what a transcript is filed under. */
	if (!hub->getUrl()) return true;

	ChatArchive::Line line;

	line.time = timestamp ? timestamp : static_cast<uint64_t>(time(0));
	line.text = message;
	line.action = action;
	line.richText = richText;
	line.privateMessage = (conversation && *conversation);

	if (user)
	{
		line.nick = user->getNick() ? user->getNick() : "";
		line.op = user->isOp();

		/*
		 * The client id where there is one, which is what identifies somebody
		 * across sessions - the session id this user is indexed by names
		 * somebody else entirely the next time they connect.
		 */
		const uint8_t* cid = user->getClientID();
		const char* text = reinterpret_cast<const char*>(cid);

		line.source = (text && *text) ? text : line.nick;
	}
	else
	{
		line.hub = hubName ? hubName : "";
	}

	return core->chat->append(hub->getUrl(), conversation ? conversation : "", line);
}
