/*
 * Copyright (C) 2001-2009 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_H
#define HAVE_QUICKDC_API_H
#include <stdio.h>
#include <string>
#include <vector>

#include "quickdc.h"
#include <samurai/io/net/socketbase.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/timer.h>


#include "api/bulletins.h"
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
class Core;
class ConnectionManager;
class HubManager;
class HubSession;
class User;
class UserManager;




/** Why a redirect a hub sent was not followed. */
enum class RedirectRefusal { None, Policy, Invalid, OtherProtocol, Downgrade, Loop };

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
	 * A reconnect has been scheduled, or there will not be one.
	 *
	 * Reported rather than left in the log, because the wait is chosen here - it
	 * doubles with each attempt and is spread over the upper half of that, so
	 * nothing outside can work out when the attempt is due by repeating the sum.
	 *
	 * Not pure: retrying is this side's business, and a listener with nowhere to
	 * show the wait has nothing to do here.
	 *
	 * @param seconds How long until the attempt, or -1 if this hub will not be
	 *                tried again - which has to be said as plainly as a wait, or a
	 *                listener counts down to an attempt that never comes.
	 */
	virtual void EventReconnectScheduled(int seconds) { (void) seconds; }

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
	 * @param timestamp When it was said, in seconds since the epoch. Always set:
	 *                  the hub's own TS where it sent one, and otherwise the
	 *                  moment it arrived here. See the note on EventHubMessage().
	 */
	virtual void EventChat(const User* user, const char* message, bool action,
	                       bool richText, uint64_t timestamp) = 0;

	/**
	 * 'user' sent you a private message.
	 *
	 * @param from The user who wrote the message
	 * @param to The user the message is intended for
	 * @param message The message
	 * @param userContextID an ID that identifies the context of a chat message (window, tab, etc). Usually the same as a user's nick name.
	 * @param action The message is to be considered as a action chat message.
	 * @param richText The message is markdown; see EventChat().
	 * @param timestamp When it was said; see EventChat().
	 */
	virtual void EventPrivateChat(const User* from, const User* to, const char* message,
	                              const char* userContextID, bool action, bool richText,
	                              uint64_t timestamp) = 0;

	/**
	 * A fake user sent a message. Usually a message from the hub upon login.
	 *
	 * @param fakeuser The 'user' who wrote the message non-existing.
	 * @param message The message
	 * @param action The message is to be considered as a action chat message.
	 * @param richText The message is markdown; see EventChat(). A hub is as entitled
	 *                 to formatting as anybody on it, and is the party that decided
	 *                 rich text is carried at all.
	 * @param timestamp When it was said, in seconds since the epoch.
	 *
	 * A hub replaying its history stamps each line with TS, so a replayed line
	 * carries the time it was originally said rather than the time it was sent
	 * again. A hub that says nothing gets the moment of arrival instead, so that
	 * every line has a comparable stamp - which is what lets a replay be lined up
	 * against what this client already wrote down.
	 */
	virtual void EventHubMessage(const char* fakeuser, const char* message,
	                             bool action, bool richText, uint64_t timestamp) = 0;
	
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
	 * The hub's own description, under its name.
	 *
	 * Separate from EventHubName() because ADC carries the two apart, in the
	 * NI and DE fields of the hub's INF. NMDC has one field for both, so a
	 * hub there says whatever it wants to say in the name and this arrives
	 * only from the $HubTopic some hub software sends as well.
	 *
	 * Not pure: this arrived after the interface did, and a listener with
	 * nowhere to show a description has nothing to do here.
	 */
	virtual void EventHubDescription(const char* description) { (void) description; }

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
	 * Whether this hub has bulletin boards, as it stands.
	 *
	 * Said after every SUP the hub sends, whether or not the answer has changed,
	 * and the current answer each time rather than a difference. That is not
	 * laziness: a hub may announce BBS0 in a later SUP than its first and may
	 * withdraw it with RMBBS0, so there is nothing to remember between the two -
	 * and a listener that had to work the state out from a stream of additions
	 * would have no shape at all in which to hear a withdrawal. A listener that
	 * draws on this compares it against what it last drew and does nothing when
	 * the two agree.
	 *
	 * False also cancels: every subscription this session held is gone with the
	 * feature, and nothing else will say so.
	 *
	 * Not pure, like the two above: a listener with nowhere to show a board has
	 * nothing to do here.
	 */
	virtual void EventBoardsAvailable(bool available) { (void) available; }

	/**
	 * The hub described one of its boards, or said that one no longer exists.
	 *
	 * One of these arrives per board on login and again whenever any field of a
	 * board changes - including what this session may do on it, which is stated
	 * per session because "what may *you* do here" is a question only the hub can
	 * answer. A descriptor subscribes nobody to anything; it says what there is
	 * to subscribe to.
	 *
	 * A board that is never described is the way a board is hidden, so there is no
	 * event for that and there cannot be one: what a listener holds is every board
	 * it has been told about, and the hub's silence is the rest of the answer.
	 *
	 * @param board with 'removed' set where the board has ceased to exist, which
	 *              also cancels any subscription this session held on it.
	 */
	virtual void EventBoardDescribed(const QuickDC::Board& board) { (void) board; }

	/**
	 * One index entry on a board: a post, or the tombstone of one.
	 *
	 * The whole of what a hub sends about a post, and it arrives for three
	 * reasons that are deliberately not distinguished - the backlog of a
	 * subscription, a post accepted from somebody since, or a post this client
	 * submitted. That last is why there is no separate acknowledgement of a
	 * submission and no success status message anywhere in this extension: the
	 * entry is the acknowledgement, the hub sends it to the author whether or not
	 * the author subscribes, and until it arrives a post is not published.
	 *
	 * Nothing about it is evidence. Everything the hub says here is a claim about
	 * a document that may not exist, and the fields copied out of that document -
	 * the parent and the subject - are hints until it has been fetched and hashed;
	 * see api/bulletins.h, which is where that is written down at length.
	 *
	 * Entries are keyed on the tth by whoever keeps them. A resumed subscription
	 * replays the second it resumed from, so a duplicate is ordinary, and a
	 * withdrawal arrives as a second entry for a tth already held carrying a later
	 * timestamp.
	 */
	virtual void EventBoardEntry(const QuickDC::BoardEntry& entry) { (void) entry; }

	/**
	 * The hub refused something said about a board.
	 *
	 * Named by the board or by the hash rather than by a token, because this
	 * extension has none: a submission is answered either by the entry or by a
	 * refusal quoting the same hash, and the two are told apart that way. So both
	 * are passed on, and a listener matches on whichever of them it was waiting
	 * for.
	 *
	 * @param code    the ADC status code, the severity stripped off. 71 is a board
	 *                that is not there and can be dropped from the interface, 72 a
	 *                post larger than the board allows, 75 a rate limit, 76 a post
	 *                the hub holds no entry for and which can stop being asked
	 *                for. Everything else is a condition to report and not to act
	 *                on, including the codes ADC already had a better word for - a
	 *                missing permission among them, since what a board descriptor
	 *                says a session may do is not a security boundary and the hub
	 *                re-checks every operation as it arrives.
	 * @param message the hub's own words, which are remote text bound for a user
	 *                interface and are neither trusted nor this side's to shorten.
	 * @param board   the board named, or empty where the refusal named none or
	 *                named one that is not a board name at all.
	 * @param tth     the post named, or empty on the same terms.
	 */
	virtual void EventBoardRefused(int code, const char* message, const char* board,
	                               const char* tth)
	{ (void) code; (void) message; (void) board; (void) tth; }

	/**
	 * The hub has sent this session to another hub.
	 * @param address   what the hub said: ADC's address whole, NMDC's host
	 * @param following the address now being connected to, empty where it is not
	 * @param why       why not, where it is not
	 */
	virtual void EventHubRedirect(const char* address, uint16_t port, const char* following,
	                              RedirectRefusal why) = 0;
	
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
 * How long a connection has to last before it counts as having worked.
 *
 * A connection that lasted this long starts the backoff again from the
 * beginning, however many attempts it took to get there: a hub this client sat
 * on for hours and was then cut off from is not a hub that is refusing it, and
 * making it wait five minutes to come back would be answering the wrong
 * question. Shorter than this and the connection is not evidence of anything -
 * a hub that accepts and immediately drops would otherwise be hammered at the
 * base interval for ever.
 */
#define QUICKDC_HUB_STABLE 30

/** How many redirects one connection follows before the next is taken for a loop. */
#define QUICKDC_HUB_MAX_REDIRECTS 4

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
		Hub(Core& core, HubListener*, Samurai::IO::Net::URL* url);
		virtual ~Hub();

		Hub(const Hub&) = delete;
		Hub& operator=(const Hub&) = delete;

		/** Connect to the address the hub was opened at, or was last sent to. */
		void connect();
		void disconnect();

		/** Whether a redirect is followed when a hub sends one. Not, until asked. */
		void setFollowRedirects(bool follow) { followRedirects = follow; }

		/**
		 * A session was sent elsewhere: ADC's address whole, NMDC's host and port.
		 * Followed where the policy allows and the address is one to go to, on the
		 * next turn of the loop, under this same Hub; the listener is told which.
		 */
		void onRedirect(const char* address, uint16_t port);

		/**
		 * Where a redirect from 'from' leads, as a hub's canonical address, or why
		 * it is not followed. 'visited' is where this connection has been sent
		 * already, the address it was opened at first: a hub may hand a client
		 * across only to the same protocol, never from adcs to adc, and not back
		 * to anywhere it has been.
		 */
		static RedirectRefusal judgeRedirect(const std::string& from, const char* address,
		                                     uint16_t port, bool follow,
		                                     const std::vector<std::string>& visited,
		                                     std::string& target);

		/**
		 * Bring the session in line with the identity the settings now give this
		 * hub. A changed nickname reconnects, since a hub knows a logged-in user
		 * by it and many refuse to have it changed under them; anything else
		 * goes out with the next INF.
		 */
		void refreshIdentity();

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

		enum class DisconnectRefusal { None, NoSession, NoSuchUser, NotOperator, Unsupported };

		/**
		 * Have the hub disconnect 'user', as an operator: sent to 'redirect' where
		 * that is not empty, and otherwise kept out for 'banSeconds' (-1 for good)
		 * where that is not 0. Why it was not asked, where it was not.
		 */
		DisconnectRefusal disconnectUser(const char* user, const std::string& message,
		                                 const std::string& redirect, int64_t banSeconds);

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

		/** Whether the user a source names - a client ID, or a nickname on NMDC - is here. */
		bool hasSourceUser(const char* cid) const;

		/**
		 * What a download source names this user by: the CID of the user with
		 * that SID or CID on ADC, the nickname itself on NMDC; empty when nobody
		 * here has it.
		 */
		std::string sourceIdOf(const char* id) const;

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

		/**
		 * Whether this hub has bulletin boards.
		 *
		 * False until the hub's SUP reply has been read, and asked again every time
		 * rather than remembered: the answer is the hub's to change while the
		 * session lasts, and EventBoardsAvailable() is where it says so.
		 */
		bool supportsBulletins() const;

		/*
		 * What a client says about a board, forwarded to the session that knows
		 * whether the hub will hear it.
		 *
		 * False means nothing was sent, and it is deliberately not reported as a
		 * system error the way an attempt to chat without a session is: a hub with
		 * no boards is the ordinary case rather than a fault, and the interface
		 * asking is the thing that was mistaken. Every argument is checked in the
		 * session before anything goes out.
		 *
		 * See network/hubsession.h for what each of them means, and in particular
		 * why a subscription resumes from the highest timestamp already held and
		 * not from one second after it.
		 */
		bool subscribeBoard(const char* board, uint64_t timestamp);
		bool unsubscribeBoard(const char* board);
		bool requestEntry(const char* board, const char* tth);
		bool submitPost(const char* tth, uint64_t size, const char* board,
		                const char* subject, const char* parent);
		bool withdrawPost(const char* tth, const char* board);

		const User* getLocalUser() const;

		/**
		 * Whether this identifier - the one a file source is keyed on - names our
		 * own session here.
		 *
		 * False before login and for a hub with no session: this answers "that is
		 * certainly us", and callers refuse things on it.
		 */
		bool isLocalUser(const char* cid) const;

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

		/**
		 * Whether a connection that lasted this many seconds counts as having
		 * worked, and so starts the backoff again from the beginning.
		 *
		 * Its own function so that the rule can be read, and tested, without
		 * waiting out a real connection to apply it to.
		 */
		static bool connectionHeld(uint64_t seconds);


		Core& getCore() const { return core; }

		/** Called by a HubManager going away first, so this does not reach back into it. */
		void forgetManager() { manager = nullptr; }

	protected:
		Core& core;
		HubManager* manager;
		HubSession* session;
		HubListener* listener;

		bool requestedDisconnect;

		/** Failed attempts in a row; zero once a connection has been established. */
		size_t attempts;

		Samurai::IO::Net::URL* url;
		char* canonicalUrl;
		Samurai::Timer* timer;

		bool followRedirects = false;
		std::vector<std::string> redirected;
		std::string redirectTarget;

		/* connect() without forgetting where redirects have taken this connection. */
		void open();

		/* By CID; only NMDC, which has none, by the nickname that is its session ID. */
		const User* sourceUser(const char* id) const;
		bool isNmdc() const;

	protected:
		void EventTimeout(Samurai::Timer*) override;

	friend class HubManager;
	friend class ClientSession;
	friend class HubSession;
	friend class DC::ClientSession;
	friend class DC::HubSession;
};

}

#endif // HAVE_QUICKDC_API_H
