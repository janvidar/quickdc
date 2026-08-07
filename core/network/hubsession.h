/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HUBSESSION_H
#define HAVE_QUICKDC_HUBSESSION_H

/* Named below, and this header is otherwise reached only through others that
   happen to have included them first. */
#include <stdint.h>
#include <time.h>

namespace QuickDC {
class Hub;
class User;
class UserManager;

namespace Share {
	class SearchRequest;
	class SearchResult;
}

/**
 * A HubSession is the API for the hub.
 * This is subclassed as DCHubSession and ADCHubSession.
 */
class HubSession {
	public:
		HubSession(Hub* ui) : hub(ui), reconnect_time(-1), established(0) { }
		virtual ~HubSession() { }

		HubSession(const HubSession&) = delete;
		HubSession& operator=(const HubSession&) = delete;

		enum UserState {
			StateNone,
			StateProtocol,
			StateIdentify,
			StateVerify,
			StateNormal,
			StateData
		};
		
		enum UserAccessLevel {
			AccessNone,
			AccessNormal,
			AccessRegistered,
			AccessOperator
		};

	public:
		virtual void connect() = 0;
		virtual void disconnect() = 0;
	
		/**
		 * Send a chat message to one specific user.
		 */
		/**
		 * @param richText the message is markdown and should be flagged as such.
		 *        Only ADC can say so; NMDC ignores it.
		 */
		virtual void sendPrivateChatMessage(const User* user, const char* message,
		                                    bool richText = false) = 0;

		/**
		 * Send a public chat message to all users.
		 */
		virtual void sendChatMessage(const char* message, bool richText = false) = 0;

		/**
		 * Kick a user.
		 * NOTE: This requires special privileges on the hub.
		 */
		virtual void sendAdminUserKick(const User* user) = 0;

		/**
		 * Redirect a user to another hub.
		 * NOTE: This requires special privileges on the hub.
		 */
		virtual void sendAdminUserRedirect(const User* user, const char* location, const char* reason) = 0;

		/**
		 * In case setting up a connection to a user fails, this is called.
		 */
		virtual void sendPeerError(const char* remote_id, const char* code, const char* msg, const char* token, const char* proto) = 0;

		/**
		 * Request connection to specific user.
		 * NOTE: If both users are in passive mode, no connection can be setup.
		 */
		virtual void sendConnectionRequest(const User* user, const char* token) = 0;

		/**
		 * Send search request.
		 */
		virtual void sendSearchRequest(QuickDC::Share::SearchRequest* request) = 0;

		/**
		 * Send local/client/status update.
		 * This should be done as often as the status actually changes, but not too
		 * often as some hubs see this as unnesesary traffic.
		 */
		virtual void sendClientInfo() = 0;
		
		

	public:
		/**
		 * Lookup a specific user.
		 * NOTE: id is not specified. For NMDC this would be a nickname, 
		 *       on ADC this would be the CID.
		 */
		virtual const User* getUser(const char* id) = 0;

		/**
		 * Get the local user.
		 */
		virtual const User* getLocalUser() = 0;
		
		/**
		 *
		 */
		virtual UserManager* getUserManager() = 0;

		/**
		 * A search result came back for us. Takes ownership of 'result'.
		 * Not always read by the session: an active client is answered by
		 * UDP straight from the peer.
		 */
		virtual void onSearchResult(Share::SearchResult* result) = 0;

		/**
		 * Reconnect in this amount of seconds,
		 * or -1 if never reconnect.
		 */
		virtual int getReconnectTime() const { return reconnect_time; }
		
		
		/**
		 * Returns true if the connection is established and we have an idle
		 * successfully logged in session on the hub.
		 */
		 virtual bool isLoggedIn() const = 0;

		/**
		 * Returns true if the traffic to the hub is encrypted.
		 *
		 * Asked of the socket every time rather than remembered from the
		 * handshake: a caller that tracked the status events instead would have
		 * to catch every one of them, and the events do not make it easy - the
		 * ADC session reports being connected again after its TLS handshake, so
		 * the last thing said about a secured connection is "connected".
		 */
		 virtual bool isSecure() const = 0;

		/**
		 * Returns true if the hub has agreed to carry markdown chat messages.
		 *
		 * False by default, because it takes a negotiated extension to say a
		 * message is markdown, and a protocol that cannot negotiate one never
		 * gets the chance: NMDC has nowhere to put the flag at all.
		 */
		 virtual bool supportsRichText() const { return false; }

		/**
		 * Supply a password after EventHubAutenticate() asked for one.
		 *
		 * Ignored unless the hub actually asked. A password is only meaningful
		 * as an answer to a request - on ADC because the answer is derived
		 * from a token the hub sent with it, and on both because a hub that
		 * did not ask has nothing to do with one.
		 *
		 * It is used for this login only and is not written to the session
		 * file; putting it there is the user's decision, not ours.
		 */
		virtual void sendPassword(const char* password) = 0;

		/**
		 * The hub this session belongs to, which is where its canonical URL
		 * comes from. A FileSource records that URL, so anything creating
		 * sources needs it.
		 */
		virtual Hub* getHub() const { return hub; }
		
		
	protected:
		/**
		 * Write one line of chat into the client's transcript of this hub.
		 *
		 * Called where the line is dispatched to the listener, so that what is
		 * kept is what the hub actually said - and so that it is kept whether or
		 * not anything is listening. A front end with no window open on a
		 * conversation is still in it.
		 *
		 * @param user who spoke, or null for a line the hub generated itself.
		 * @param hubName what to call the hub when 'user' is null.
		 * @param conversation who a private message is with, in the form that
		 *        outlasts the session - the client id on ADC, the nickname on
		 *        NMDC. Empty for public chat, which is the hub's own transcript.
		 * @param timestamp when it was said, in seconds since the epoch - the
		 *        hub's own TS where it sent one. Taken rather than read from the
		 *        clock here, so that a hub replaying its history is written down
		 *        under the times it gives rather than all at once on login.
		 *
		 * @return false when this line was already in the transcript, which is
		 *         what a hub replaying history it has already been given looks
		 *         like. The caller should not report it onwards either: it was
		 *         shown when it first arrived, and the conversation it belongs to
		 *         was filled from the same transcript on the way in.
		 */
		bool recordChat(const User* user, const char* hubName, const char* message,
		                const char* conversation, bool action, bool richText,
		                uint64_t timestamp);

	public:
		/**
		 * How long this session has been logged in, in seconds, or zero if it
		 * never got that far.
		 *
		 * Asked as the connection ends, to decide whether it lasted long enough
		 * to count as having worked - see QUICKDC_HUB_STABLE. Kept here rather
		 * than read from isLoggedIn(), which by then says no: every path that
		 * ends a session clears the state before reporting the disconnection.
		 */
		uint64_t establishedFor() const;

		/**
		 * Whether this session ever got as far as being logged in.
		 *
		 * Distinct from establishedFor() being zero, which is also what a session
		 * logged in less than a second ago reports.
		 */
		bool wasEstablished() const;

	protected:
		/** Note that this session is now logged in. Called once, on getting there. */
		void markEstablished();

		enum UserAccessLevel access;
		Hub* hub;
		int reconnect_time;

		/** When markEstablished() was called, or zero; see establishedFor(). */
		time_t established;
		
	friend class HubManager;
};

}

#endif // HAVE_QUICKDC_HUBSESSION_H

