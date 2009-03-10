/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/core.h"
#include "api/hub.h"
#include "network/hubmanager.h"
#include "network/dc/commands.h"
#include "network/dc/textescape.h"

DC::SendValidateNick::SendValidateNick(const char* nick) : DC::Command()
{
	buffer->append("$ValidateNick ");
	buffer->append(nick);
	buffer->append('|');
}

DC::SendMyPass::SendMyPass(const char* password) : DC::Command()
{
	buffer->append("$MyPass ");
	buffer->append(password);
	buffer->append('|');
}

DC::SendVersion::SendVersion() : DC::Command()
{
	buffer->append("$Version 1,0091|");
}

DC::SendGetNickList::SendGetNickList() : DC::Command()
{
	buffer->append("$GetNickList|");
}

DC::SendSupports::SendSupports() : DC::Command()
{
	buffer->append("$Supports ");
	buffer->append("UserCommand ");
	buffer->append("NoGetINFO ");
	buffer->append("NoHello ");
	buffer->append("UserIP2 ");
	buffer->append("TTHSearch ");
	buffer->append("GetZBlock ");
#ifdef SSL_SUPPORT
	buffer->append("SSL ");
#endif
	buffer->append('|');
}

DC::SendKey::SendKey(const char* key, size_t len) : DC::Command()
{
	buffer->append("$Key ");
	buffer->append(key, len);
	buffer->append('|');
}

DC::SendGetInfo::SendGetInfo(const char* nick, const char* me) : DC::Command()
{
	buffer->append("$GetINFO ");
	buffer->append(nick);
	buffer->append(' ');
	buffer->append(me);
	buffer->append('|');
}


DC::SendMyInfo::SendMyInfo(const char* nick, const char* description, const char* email, uint64_t shared_size) : DC::Command()
{
	const char* connection = "10";  // FIXME
	char flags = 0x01;              // FIXME
	bool active_mode = false;       // FIXME
	int  numslots = 3;              // FIXME
	
	char* tag = (char*) malloc(50);
	snprintf(tag, 50, "<" PRODUCT " V:" VERSION ",M:%c,H:%d/%d/%d,S:%d>",
			active_mode ? 'A' : 'P',
			(int) QuickDC::Core::getInstance()->hubs->countHubsNormal()+1,
			(int) QuickDC::Core::getInstance()->hubs->countHubsRegistered(),
			(int) QuickDC::Core::getInstance()->hubs->countHubsOperator()+1, numslots
		);
	
	buffer->append("$MyINFO $ALL ");
	buffer->append(DC::TextEscape::escape(nick));
	buffer->append(' ');
	buffer->append(DC::TextEscape::escape(description));
	buffer->append(tag);
	buffer->append("$ $");
	buffer->append(connection);
	buffer->append(flags);
	buffer->append("$");
	buffer->append(DC::TextEscape::escape(email));
	buffer->append("$");
	buffer->append(shared_size);
	buffer->append("$|");
	
	free(tag);
}

DC::SendTo::SendTo(const char* from, const char* to, const char* message) : DC::Command()
{
	buffer->append("$To: ");
	buffer->append(to);
	buffer->append(" From: ");
	buffer->append(from);
	buffer->append(" $<");
	buffer->append(from);
	buffer->append("> ");
	buffer->append(DC::TextEscape::escape(message));
	buffer->append('|');
}

DC::SendMessage::SendMessage(const char* from, const char* message) : DC::Command()
{
	buffer->append("<");
	buffer->append(from);
	buffer->append("> ");
	buffer->append(DC::TextEscape::escape(message));
	buffer->append('|');
}

DC::SendKick::SendKick(const char* user) : DC::Command()
{
	buffer->append("$Kick ");
	buffer->append(user);
	buffer->append('|');
}

DC::SendRedirect::SendRedirect(const char* user, const char* address, const char* message) : DC::Command()
{
	buffer->append("$OpForceMove $Who:");
	buffer->append(user);
	buffer->append("$Where:");
	buffer->append(address);
	buffer->append("$Msg:");
	buffer->append(DC::TextEscape::escape(message));
	buffer->append('|');
}

DC::SendConnectToMe::SendConnectToMe(const char* user, const char* address, uint16_t port) : DC::Command()
{
	buffer->append("$ConnectToMe ");
	buffer->append(user);
	buffer->append(' ');
	buffer->append(address);
	buffer->append(':');
	buffer->append(port);
	buffer->append('|');
}

DC::SendRevConnectToMe::SendRevConnectToMe(const char* from, const char* to) : DC::Command()
{
	buffer->append("$RevConnectToMe ");
	buffer->append(from);
	buffer->append(' ');
	buffer->append(to);
	buffer->append('|');
}

DC::SendEmptyCommand::SendEmptyCommand() : DC::Command()
{
	buffer->append('|');
}

DC::SendMyNick::SendMyNick(const char* nick) : DC::Command()
{
	buffer->append("$MyNick ");
	buffer->append(nick);
	buffer->append('|');
}

DC::SendLock::SendLock(const char* lock, const char* pk) : DC::Command()
{
	buffer->append("$Lock ");
	buffer->append(lock);
	buffer->append(" Pk=");
	buffer->append(pk);
	buffer->append('|');
}

DC::SendClientSupports::SendClientSupports() : DC::Command()
{
	buffer->append("$Supports ");
	buffer->append("MiniSlots "); /* Upload filelist even when all slots are occupied */
	buffer->append("XmlBZList "); /* Support XML extended file list */
	buffer->append("ADCGet ");    /* More streamlined and polished GET mechanism */
	buffer->append("TTHL ");      /* ADCGet: transfer intermediate leaf hashes */
	buffer->append("TTHF ");      /* ADCGet: Lookup file on the form TTH/{base32 sum} */
	buffer->append("ZLIG ");      /* ADCGet with zlib transfers */

	// Obsoleted stuff:
	// buffer->append("BZList ");    /* BZ2 compressed file list (MyList.bz2) - obsoleted for XmlBZList */
	// buffer->append("GetZBlock "); /* Zlib transfers are supported  - obsoleted for ADCGet */
	// buffer->append("CHUNK ");     /* DISALBED - no need to have it */
	buffer->append("|");
}

DC::SendDirection::SendDirection(bool download, size_t priority) : DC::Command()
{
	buffer->append("$Direction ");
	buffer->append( download ? "Download " : "Upload " );
	buffer->append(quickdc_itoa(priority, 10));
	buffer->append('|');
}

DC::SendErrorFileNotAvailable::SendErrorFileNotAvailable() : DC::Command()
{
	buffer->append("$Error File Not Available|");
}

DC::SendFileLength::SendFileLength(uint64_t size) : DC::Command()
{
	buffer->append("$FileLength ");
	buffer->append(size);
	buffer->append('|');
}

DC::SendListLength::SendListLength(uint64_t size) : DC::Command()
{
	buffer->append("$ListLen ");
	buffer->append(size);
	buffer->append('|');
}
