/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "test/cli/hubconnection.h"
#include "test/cli/util.h"
#include "test/cli/console.h"

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
	const char* msg;
	
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

void HubConnection::EventChat(const QuickDC::User* user, const char* message, bool action)
{
	if (action)
	{
		console_printf("%s      *%s* %s\n", getTimestamp(), user->getNick(), message);
	}
	else
	{
		console_printf("%s      <%s> %s\n", getTimestamp(), user->getNick(), message);
	}
}

void HubConnection::EventPrivateChat(const QuickDC::User* from, const QuickDC::User* to, const char* message, const char* context, bool action) {
	
	if (!action)
		console_printf("%s * {%s} Message to %s: <%s> %s\n", getTimestamp(), context, to->getNick(), from->getNick(), message);
	else
		console_printf("%s * {%s} Message to %s: *%s %s*\n", getTimestamp(), context, to->getNick(), from->getNick(), message);
}

void HubConnection::EventHubMessage(const char* user, const char* message) {
	console_printf("%s * HUB: <%s> %s\n", getTimestamp(), user, message);
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

void HubConnection::EventSearchResult(void*)
{
	console_printf("search result\n");
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

