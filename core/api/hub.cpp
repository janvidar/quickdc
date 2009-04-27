/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdio.h>
#include <string.h>

#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketbase.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/url.h>
#include "network/user.h"
#include "network/dc/dccommandparser.h"
#include "network/dc/dccommandproxy.h"
#include "network/dc/dcconnection.h"
#include "network/dc/hubcommands.h"
#include "network/dc/hubsession.h"
#include "network/adc/adccommandparser.h"
#include "network/adc/adccommandproxy.h"
#include "network/adc/client/adchubsession.h"
#include "network/connectionmanager.h"
#include "network/hubmanager.h"
#include "api/hub.h"
#include "api/core.h"
#include "config/preferences.h"

QuickDC::Hub::Hub(HubListener* listener_, Samurai::IO::Net::URL* url_)
	: session(0)
	, listener(listener_)
	, url(new Samurai::IO::Net::URL(url_))
	, timer(0)
{
	QuickDC::Core::getInstance()->hubs->add(this);
}

QuickDC::Hub::~Hub()
{
	delete url;
	
	if (QuickDC::Core::getInstance())
	{
		QuickDC::Core::getInstance()->hubs->remove(this);
	}
}

void QuickDC::Hub::connect()
{
	if (session)
	{
	        requestedDisconnect = true;
	        session->disconnect();
	}
	
	std::string scheme = url->getScheme();
	
	/**
	 * TODO: Create a factory class that will select the apropriate one.
	 */
	if (scheme == "dchub")
	{
		session = new DC::HubSession(this, listener, url);
	}
	else if (scheme == "adcs")
	{
		session = new ADC::HubSession(this, listener, url, true);
	}
	else if (scheme == "adc")
	{
		session = new ADC::HubSession(this, listener, url, false);
	}
	else
	{
		QERR("Protocol not handled. URI: '%s'", url->toString().c_str());
		return;
	}

	requestedDisconnect = false;
	delete timer;
	timer = 0;
	
	if (session)
	{
		session->connect();
	}
}

void QuickDC::Hub::disconnect()
{
	requestedDisconnect = true;
	if (session)
	{
		session->disconnect();
	}
}

void QuickDC::Hub::sendChatMessage(const char *message)
{
	if (session && session->isLoggedIn())
	{
		session->sendChatMessage(message);
	}
	else
	{
		listener->EventSystemError("sendChatMessage(): No session");
	}
}

void QuickDC::Hub::sendPrivateChatMessage(const QuickDC::User* user, const char* message) {
	if (!session && session->isLoggedIn())
	{
		listener->EventSystemError("sendPrivateChatMessage(): No session");
		return;
	}

	if (!user)
	{
		listener->EventSystemError("sendPrivateChatMessage(): No such user");
		return;
	}

	if (!message)
	{
		listener->EventSystemError("sendPrivateChatMessage(): Message is null");
		return;
	}

	if (!strlen(message))
	{
		listener->EventSystemError("sendPrivateChatMessage(): Message is empty");
		return;
	}

	session->sendPrivateChatMessage(user, message);
}

void QuickDC::Hub::sendKickUser(const char* user)
{
	if (!session || !session->isLoggedIn())
	{
		listener->EventSystemError("sendKickUser(): No session");
		return;
	}
	
	const QuickDC::User* u = session->getUser(user);
	if (u)
		session->sendAdminUserKick(u);
	else
		listener->EventSystemError("sendKickUser(): No such user");
}

void QuickDC::Hub::sendRedirect(const char* user, const char* address, const char* reason)
{
	if (!session || !session->isLoggedIn())
	{
		listener->EventSystemError("sendRedirect(): No session");
		return;
	}
	
	const QuickDC::User* u = session->getUser(user);
	if (u)
		session->sendAdminUserRedirect(u, address, reason);
	else
		listener->EventSystemError("sendRedirect(): No such user");
}

void QuickDC::Hub::connectUser(const char* user)
{
	if (!session || !session->isLoggedIn())
	{
		listener->EventSystemError("connectUser(): No session");
		return;
	}
	const QuickDC::User* u = session->getUser(user);
	if (u) /* FIXME: Generate a token */
		session->sendConnectionRequest(u, "TOKENIZEDFIXME");
	else
		listener->EventSystemError("connectUser(): No such user");
}

const QuickDC::User* QuickDC::Hub::getLocalUser() const
{
	return (session) ? session->getLocalUser() : 0;
}

QuickDC::UserManager* QuickDC::Hub::getUserManager() const
{
	return (session) ? session->getUserManager() : 0;
}

void QuickDC::Hub::sendSearchRequest(Share::SearchRequest* request)
{
	if (!session || !session->isLoggedIn())
	{
		listener->EventSystemError("sendSearchRequest(): No session");
		return;
	}
	
	if (!request)
	{
		listener->EventSystemError("sendSearchRequest(): No request");
		return;
	}
	session->sendSearchRequest(request);
}

void QuickDC::Hub::EventTimeout(Samurai::Timer*)
{
	delete timer; timer = 0;
	connect();
}

void QuickDC::Hub::onDisconnected()
{
	if (session && session->getUserManager())
	{
		listener->EventUsersCleanup();
		session->getUserManager()->cleanup();
	}

	int reconnect_time = (session) ? session->getReconnectTime() : -1;
	session = 0;
	if (!requestedDisconnect && reconnect_time != -1)
	{
		timer = new Samurai::Timer(this, reconnect_time, true);
	}
}

