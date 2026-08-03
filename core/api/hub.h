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

			/**
			 * Nothing was reachable: no route to the host, or the network is
			 * down. Distinct from the errors above because it is the one that
			 * is often a moment old - a route that has not come up yet, or on
			 * macOS the first attempt at a local address, which is spent
			 * asking whether this application may make it at all.
			 */
			NetErrorHostUnreachable,

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
	 * @param richText The sender flagged the text as markdown (ADC's RT1). Without
	 *                 it the message is literal, which is what lets an asterisk
	 *                 stay an asterisk.
	 */
	virtual void EventChat(const User* user, const char* message, bool action = false,
	                       bool richText = false) = 0;

	/**
	 * 'user' sent you a private message.
	 *
	 * @param from The user who wrote the message
	 * @param to The user the message is intended for
	 * @param message The message
	 * @param userContextID an ID that identifies the context of a chat message (window, tab, etc). Usually the same as a user's nick name.
	 * @param action The message is to be considered as a action chat message.
	 * @param richText The message is markdown; see EventChat().
	 */
	virtual void EventPrivateChat(const User* from, const User* to, const char* message, const char* userContextID, bool action, bool richText = false) = 0;

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
	 * The hub announced an extension in its SUP reply.
	 *
	 * Called once per extension, as the session reads them, which is how a
	 * client learns whether something like RTF0 is available.
	 *
	 * @param fourcc The extension name as the hub gave it, without the AD
	 *               prefix, for instance "RTF0".
	 */
	virtual void EventHubExtension(const char* fourcc) { (void) fourcc; }

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

/** How long after a connection ended the first attempt to restore it is made. */
#define QUICKDC_HUB_RETRY_BASE 5

/** And the longest wait between attempts, however many have failed. */
#define QUICKDC_HUB_RETRY_MAX 300

/**
 * A host that could not be reached is retried this soon, and only once.
 *
 * Short because the usual cause is a route, or a permission, that was a moment
 * away from being there - see NetErrorHostUnreachable.
 */
#define QUICKDC_HUB_RETRY_UNREACHABLE 1

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

		/**
		 * @param richText the text is markdown, and the message should carry the RT
		 *        flag saying so. ADC only; NMDC has nowhere to put it.
		 */
		void sendChatMessage(const char* message, bool richText = false);

		/**
		 * Answer a hub that asked for a password, after the listener was told
		 * through EventHubAutenticate().
		 */
		void sendPassword(const char* password);
		void sendPrivateChatMessage(const QuickDC::User* user, const char* message,
		                            bool richText = false);
		void sendKickUser(const char* user);
		void sendRedirect(const char* user, const char* address, const char* reason);

		/**
		 * Ask a user to set up a peer connection.
		 *
		 * @param user  the user ID; a CID on ADC, a nickname on NMDC.
		 * @param token identifies what the resulting connection is for, so
		 *              that the client session can look the job up in the
		 *              download queue. One is generated if this is 0.
		 * @return false if there is no session, or no such user.
		 */
		bool connectUser(const char* user, const char* token = 0);

		/**
		 * The same, but identified the way the download queue stores a user:
		 * by client ID on ADC, by nickname on NMDC.
		 *
		 * connectUser() takes a session ID, which is only valid while the user
		 * stays on the hub and so is no use to a queue that outlives that.
		 * Failures are logged rather than reported to the listener, because the
		 * queue retries and would otherwise report the same problem repeatedly.
		 */
		bool connectUserByClientID(const char* cid, const char* token);

		/**
		 * The hub URL in canonical "{scheme}://{host}:{port}" form.
		 * This is what a FileSource records, so the two have to agree.
		 */
		const char* getUrl() const;

		/**
		 * Whether the traffic to this hub is encrypted, right now.
		 *
		 * False when there is no session, which is also the honest answer: there
		 * is nothing to protect and nothing protecting it.
		 */
		bool isSecure() const;

		/**
		 * Whether this hub has agreed to carry markdown.
		 *
		 * False until the hub's SUP reply has been read, and false forever on
		 * a hub that did not announce the extension.
		 */
		bool supportsRichText() const;

		const User* getLocalUser() const;
		UserManager* getUserManager() const;

		void sendSearchRequest(Share::SearchRequest* request);

		/**
		 * Hand a search result to this hub's session. Takes ownership of
		 * 'result', and deletes it if there is no session to give it to.
		 */
		void onSearchResult(Share::SearchResult* result);


		/* Internal */

		/**
		 * The session has gone.
		 *
		 * @param why what took it away, which is what decides whether and when to
		 *        try again - see retryDelay().
		 */
		void onDisconnected(HubListener::StatusNetwork why =
		                    HubListener::NetDisconnected);

		/**
		 * How long to wait before trying again, in seconds, or -1 to give up.
		 *
		 * Nothing unreachable is retried more than once: a route that is not there
		 * is usually either a moment old - and a second is enough for it - or it is
		 * not coming back, and asking every few seconds for the rest of the session
		 * would say nothing new. Everything else that ends a connection is worth
		 * coming back to, so those back off: each attempt waits about twice as long
		 * as the one before, up to a ceiling.
		 *
		 * The wait is spread over a range rather than taken exactly, so that a
		 * hundred clients thrown off the same hub at the same moment do not all come
		 * back at the same moment and throw it off again.
		 *
		 * @param why the reason the connection ended.
		 * @param attempts how many have failed in a row, starting at 1 for the first.
		 *
		 * Static so that the policy can be read and tested on its own.
		 */
		static int retryDelay(HubListener::StatusNetwork why, size_t attempts);


	protected:
		HubSession* session;
		HubListener* listener;

		bool requestedDisconnect;

		/** Failed attempts in a row; zero once a connection has been established. */
		size_t attempts;

		Samurai::IO::Net::URL* url;
		char* canonicalUrl;
		Samurai::Timer* timer;
		
		void EventTimeout(Samurai::Timer*) override;

	friend class HubManager;
	friend class ClientSession;
	friend class HubSession;
	friend class DC::ClientSession;
	friend class DC::HubSession;
};

}

#endif // HAVE_QUICKDC_API_H
