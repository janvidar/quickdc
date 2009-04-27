/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_H
#define HAVE_QUICKDC_API_H
#include <stdio.h>

#include "quickdc.h"
#include <samurai/io/net/socketbase.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/timer.h>


#include "network/hubsession.h"
#include "network/user.h"
#include "config/preferences.h"
#include "share/sharemanager.h"


namespace DC {
class ClientSession;
class HubSession;
}

namespace Samurai {
namespace IO {
namespace Net {
class URL;
}
}
}

namespace Search {
class SearchRequest;
}

namespace QuickDC {
class ClientSession;
class CommandProxy;
class ConnectionManager;
class HubManager;
class HubSession;
class User;
class UserManager;




class HubListener
{
	public:
		virtual ~HubListener() {}
		
		enum StatusNetwork
		{
			NetInvalid,
			NetLookup,
			NetConnecting,
			NetConnected,
			NetDisconnected,
			NetTlsConnected,
			NetTlsDisconnected,
			NetErrorTimeout,
			NetErrorConnectionRefused,
			NetErrorHostNotFound,
			NetErrorIO,
			NetTlsError
		};
		
		enum StatusHub
		{
			HubNone,
			HubLogin,
			HubPassword,
			HubLoggedIn,
			HubErrorLoginRefused,
			HubErrorFull,
			HubErrorDisabled,
			HubErrorNickNotAccepted,
			HubErrorNickTaken,
			HubErrorCIDTaken,
			HubErrorWrongPassword,
			HubKicked,
			HubBannedTemporarily,
			HubBannedPermanently,
			HubRedirected,
		};

		enum ChatType
		{
			ChatPublic,
			ChatPrivate,
			ChatMessageIsBotGenerated,
			ChatAction
		};

	/**
	 * Both network and hub related status messages are handled here.
	 * Note, some events are pushed multiple times using this scheme.
	 *
	 * @param netstate The state of the network connection 
	 * @param message  A message stating the recent event, not always used.
	 */
	virtual void EventNetStatus(enum StatusNetwork netstate) = 0;
	
	/**
	 * Tell the UI about specific events.
	 */
	virtual void EventHubStatus(enum StatusHub hubstate) = 0;

	/**
	 * Notify a system error
	 */
	virtual void EventSystemError(const char* msg) = 0;
	
	/**
	 * 'user' sent a public message to everybody on the hub.
	 *
	 * @param user The user who wrote the message.
	 * @param message The message
	 */
	virtual void EventChat(const User* user, const char* message, bool action = false) = 0;

	/**
	 * 'user' sent you a private message.
	 *
	 * @param from The user who wrote the message
	 * @param to The user the message is intended for
	 * @param message The message
	 * @param userContextID an ID that identifies the context of a chat message (window, tab, etc). Usually the same as a user's nick name.
	 * @param action The message is to be considered as a action chat message.
	 */
	virtual void EventPrivateChat(const User* from, const User* to, const char* message, const char* userContextID, bool action) = 0;

	/**
	 * A fake user sent a message. Usually a message from the hub upon login.
	 *
	 * @param fakeuser The 'user' who wrote the message non-existing.
	 * @param message The message
	 */
	virtual void EventHubMessage(const char* fakeuser, const char* message) = 0;
	
	/**
	* A user joined the hub. At this point, we don't have any user 
	* data, other than the nick of the user.
	*/
	virtual void EventUserJoin(const char* user) = 0;

	/**
	 * A user left the hub
	 */
	virtual void EventUserLeave(const QuickDC::User* user, const char* message = 0, bool disconnect = false) = 0;

	/**
	 * Stop referencing pointers to QuickDC::User objects.
	 */
	virtual void EventUsersCleanup() = 0;

	/**
	 * A user has updated his/hers data.
	 * This typically happens if the description,
	 * tag or share has changed.
	 * Currently DC doesn't support name changes,
	 * but that can be accomplished by this too.
	 */
	virtual void EventUserUpdate(const User* u) = 0;

	/**
	 * Search result just came in.
 	 */
	virtual void EventSearchResult(void*) = 0;
	
	/**
	 * This holds the hubname, or often topic of the hub.
	 */
	virtual void EventHubName(const char* hubname) = 0;
	
	/**
	 * The hub has redirected you to another hub.
	 * @return true to follow redirect, or false to ignore it
	 */
	virtual bool EventHubRedirect(const char* address, uint16_t port) = 0;
	
	/**
	 * The hub requires authentication.
	 * You must call CoreHub::authenticate()
	 */
	virtual void EventHubAutenticate() = 0;
	
	/**
	 * The hub has requested you to connect to another user
	 * at the given address, port etc.
	 *
	 * @param address IP-adress (not hostname)
	 * @param port port of user
	 * @param username Might be 0 on servers without IP-information.
	 * @return true to connect to this user or false to ignore request.
	 */
	virtual bool EventClientConnect(const char* address, uint16_t port, const User* user) = 0;
	
	/**
	 * The hub has requested you to ask another user to connect to you.
	 * The reason for this is that this other user is behind a firewall,
	 * and can connect outbound, but is not able to accept incomming 
	 * connections, this is called passive mode.
	 * 
	 * If you are also in passive mode, it is not possible to setup a
	 * direct connection between the two peers, and this event is
	 * only informal.
	 *
	 * @param user the user who requested the connection
	 * @return true to setup connection, or false to ignore request.
	 */
	virtual bool EventClientConnect(const User* user) = 0;
};

class DCServerListener
{
	public:
		virtual ~DCServerListener() {}
		virtual bool EventGotConnection(const char* address, uint16_t port) = 0;
};

/**
 * Subclass this to receive hub state information.
 */
class Hub
	: public Samurai::TimerListener
{

	public:
		Hub(HubListener*, Samurai::IO::Net::URL* url);
		virtual ~Hub();

		void connect();
		void disconnect();

		void sendChatMessage(const char* message);
		void sendPrivateChatMessage(const QuickDC::User* user, const char* message);
		void sendKickUser(const char* user);
		void sendRedirect(const char* user, const char* address, const char* reason);
		void connectUser(const char* user);
		const User* getLocalUser() const;
		UserManager* getUserManager() const;

		void sendSearchRequest(Share::SearchRequest* request);
		
		/* Internal */
		void onDisconnected();


	protected:
		HubSession* session;
		HubListener* listener;

		bool requestedDisconnect;
		Samurai::IO::Net::URL* url;
		Samurai::Timer* timer;
		
		void EventTimeout(Samurai::Timer*);

	friend class HubManager;
	friend class ClientSession;
	friend class HubSession;
	friend class DC::ClientSession;
	friend class DC::HubSession;
};

}

#endif // HAVE_QUICKDC_API_H
