/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "test/cli/hubconnection.h"
#include "test/cli/util.h"
#include "test/cli/console.h"
#include "test/cli/commands.h"
#include "share/searchresult.h"
#include "network/user.h"

HubConnection::HubConnection(Samurai::IO::Net::URL* url) : QuickDC::Hub(this, url) {
	showjoins = true;
	allowConnections = true;
}

HubConnection::~HubConnection() {

}

void HubConnection::status(const char* string, const char* string2) {
	console_printf("%s * HUB: %s %s\n", getTimestamp(), string, string2);
}

void HubConnection::EventNetStatus(enum StatusNetwork netstate) {
	/* Initialised, because a state added to the enum and not here would
	   otherwise be reported by printing an uninitialised pointer. */
	const char* msg = "Unknown network state";

	switch (netstate)
	{
		case NetInvalid:
			msg = "Invalid network address";
			break;
		
		case NetLookup:
			msg = "Looking up host...";
			break;
			
		case NetConnecting:
			msg = "Connecting...";
			break;
				
		case NetConnected:
			msg = "Connected";
			break;
			
		case NetDisconnected:
			msg = "Disconnected";
			break;
			
		case NetTlsConnected:
			msg = "TLS Connected (line is secure)";
			break;
		
		case NetTlsDisconnected:
			msg = "TLS Disconnected (line is no longer secure)";
			break;

		case NetErrorTimeout:
			msg = "Error: Connection timed out";
			break;
			
		case NetErrorConnectionRefused:
			msg = "Error: Connection refused";
			break;
		
		case NetErrorHostNotFound:
			msg = "Error: Host not found";
			break;

		case NetErrorHostUnreachable:
			msg = "Error: No route to the host, or the network is down";
			break;

		case NetErrorIO:
			msg = "Error: An IO error occured";
			break;
			
		case NetTlsError:
			msg = "Error: A TLS error occured";
			break;
	};
	console_printf("%s * NET: %s\n", getTimestamp(), msg);
}


void HubConnection::EventHubStatus(enum StatusHub) {
	// FIXME: Add this!
}

void HubConnection::EventSystemError(const char* msg)
{
	console_printf("%s * ERROR: %s\n", getTimestamp(), msg ? msg : "");
}

void HubConnection::EventChat(const QuickDC::User* user, const char* message, bool action,
                              bool richText)
{
	/* A console shows the markers as they arrived; that is markdown degrading. */
	(void) richText;

	if (action)
	{
		console_printf("%s      *%s* %s\n", getTimestamp(), user->getNick(), message);
	}
	else
	{
		console_printf("%s      <%s> %s\n", getTimestamp(), user->getNick(), message);
	}
}

void HubConnection::EventPrivateChat(const QuickDC::User* from, const QuickDC::User* to, const char* message, const char* context, bool action, bool richText) {
	(void) richText;

	
	if (!action)
		console_printf("%s * {%s} Message to %s: <%s> %s\n", getTimestamp(), context, to->getNick(), from->getNick(), message);
	else
		console_printf("%s * {%s} Message to %s: *%s %s*\n", getTimestamp(), context, to->getNick(), from->getNick(), message);
}

void HubConnection::EventHubMessage(const char* user, const char* message, bool action,
                                    bool richText) {
	/* A console shows the markers as they arrived; that is markdown degrading. */
	(void) richText;

	if (!action)
		console_printf("%s * HUB: <%s> %s\n", getTimestamp(), user, message);
	else
		console_printf("%s * HUB: *%s %s*\n", getTimestamp(), user, message);
}

void HubConnection::EventUserJoin(const char* user)
{
	if (showjoins)
		status("userjoin", user);
}

void HubConnection::EventUserLeave(const QuickDC::User* user, const char* message, bool)
{
	if (showjoins) {
		if (message) 
			console_printf("%s * HUB: userquit: %s (%s)\n", getTimestamp(), user->getNick(), message);
		else
			console_printf("%s * HUB: userquit: %s\n", getTimestamp(), user->getNick());
	}
}

void HubConnection::EventUserUpdate(const QuickDC::User*)
{

}

void HubConnection::EventUsersCleanup()
{

}

void HubConnection::EventSearchResult(void* ptr)
{
	QuickDC::Share::SearchResult* result = (QuickDC::Share::SearchResult*) ptr;
	if (!result) return;

	const QuickDC::User* user = result->getUser();

	/* Recorded rather than just printed, so /download can refer to it. */
	cmd_addSearchResult(result->getFile(), result->getSize(), result->getTTH(),
		user ? user->getNick() : "?",
		user ? (const char*) user->getClientID() : 0,
		result->getToken());
}

void HubConnection::EventHubName(const char* hubname)
{
	status("hubname", hubname);
}

bool HubConnection::EventHubRedirect(const char*, uint16_t)
{
	return false;
}

void HubConnection::EventHubAutenticate()
{
	console_printf("%s * HUB: this hub wants a password; send one with /password <password>\n",
		getTimestamp());
}

bool HubConnection::EventClientConnect(const char* addr, uint16_t port, const QuickDC::User* u)
{
	const char* who = (u) ? u->getNick() : addr;
	console_printf("%s * NET: %s requested active connection [%s:%d]\n", getTimestamp(), who, addr, port);
	return allowConnections;
}

bool HubConnection::EventClientConnect(const QuickDC::User* u)
{
	console_printf("%s * NET: %s requested passive connection\n", getTimestamp(), u->getNick());
	return allowConnections;
}

