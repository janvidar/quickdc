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
#include "share/searchresult.h"

QuickDC::Hub::Hub(HubListener* listener_, Samurai::IO::Net::URL* url_)
	: session(0)
	, listener(listener_)
	, url(new Samurai::IO::Net::URL(url_))
	, canonicalUrl(0)
	, timer(0)
{
	/*
	 * Build the canonical form once. URL::toString() keeps whatever path the
	 * user typed, and a FileSource stores only "{scheme}://{host}:{port}", so
	 * the two would not compare equal without this.
	 */
	std::string canonical = url->getScheme();
	canonical += "://";
	canonical += url->getHost().toString();
	canonical += ":";
	canonical += std::to_string(url->getPort());
	canonicalUrl = strdup(canonical.c_str());

	QuickDC::Core::getInstance()->hubs->add(this);
}

QuickDC::Hub::~Hub()
{
	delete url;
	free(canonicalUrl);

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

void QuickDC::Hub::sendPassword(const char* password)
{
	/* Deliberately not guarded on isLoggedIn(): being asked for a password is
	   precisely the state of not being logged in yet. */
	if (session)
		session->sendPassword(password);
	else
		listener->EventSystemError("sendPassword(): No session");
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

bool QuickDC::Hub::connectUser(const char* user, const char* token)
{
	if (!session || !session->isLoggedIn())
	{
		listener->EventSystemError("connectUser(): No session");
		return false;
	}

	const QuickDC::User* u = session->getUser(user);
	if (!u)
	{
		listener->EventSystemError("connectUser(): No such user");
		return false;
	}

	if (token)
	{
		session->sendConnectionRequest(u, token);
		return true;
	}

	/*
	 * Nothing in the download queue is behind this one - it is a manual
	 * connect - so there is no job to correlate and any unique token will do.
	 */
	static size_t counter = 0;
	char generated[32];
	snprintf(generated, sizeof(generated), "MAN%lu%lu",
		(unsigned long) Samurai::TimeStamp().getInternalData(),
		(unsigned long) ++counter);

	session->sendConnectionRequest(u, generated);
	return true;
}

bool QuickDC::Hub::connectUserByClientID(const char* cid, const char* token)
{
	if (!session || !session->isLoggedIn())
	{
		QDBG("connectUserByClientID(): not logged in");
		return false;
	}

	if (!cid || !*cid || !token) return false;

	QuickDC::UserManager* users = session->getUserManager();
	if (!users) return false;

	const QuickDC::User* u = users->getUserByClientID(cid);

	/*
	 * NMDC users have no client ID, and there the nickname doubles as the
	 * session ID, so fall back to the plain lookup.
	 */
	if (!u) u = session->getUser(cid);

	if (!u)
	{
		QDBG("connectUserByClientID(): '%s' is not on this hub", cid);
		return false;
	}

	session->sendConnectionRequest(u, token);
	return true;
}

const char* QuickDC::Hub::getUrl() const
{
	return canonicalUrl;
}

bool QuickDC::Hub::isSecure() const
{
	return session && session->isSecure();
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

void QuickDC::Hub::onSearchResult(Share::SearchResult* result)
{
	if (!result) return;

	if (!session)
	{
		delete result;
		return;
	}

	session->onSearchResult(result);
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

