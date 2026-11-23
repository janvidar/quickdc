/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HUBSESSION_H
#define HAVE_QUICKDC_HUBSESSION_H

/* Named below, and this header is otherwise reached only through others that
   happen to have included them first. */
#include <stdint.h>
#include <string>
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

		/** The nickname this session goes by on the hub. */
		virtual std::string currentNick() const = 0;

		/**
		 * Take on this identity for what is sent from now on. The nickname is
		 * taken only before logging in: once logged in, a hub knows us by it,
		 * and changing it is Hub::refreshIdentity()'s business.
		 */
		virtual void setIdentity(const std::string& nick, const std::string& description,
		                         const std::string& email) = 0;
		
		

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
		 * Returns true if this hub has agreed to carry bulletin boards.
		 *
		 * False by default for the reason supportsRichText() is: it takes a
		 * negotiated extension to send a bulletin at all, and NMDC can negotiate
		 * nothing of the kind. Asked again wherever it matters rather than
		 * remembered anywhere, because the hub's answer is authoritative for the
		 * connection and may change while it lasts - a hub may announce the
		 * feature in a later SUP than its first, and may take it back.
		 */
		 virtual bool supportsBulletins() const { return false; }

		/*
		 * The five things a client says about a bulletin board, each doing nothing
		 * and answering false where the protocol has no way to say it.
		 *
		 * False means nothing was sent, and it is not an error: a hub without
		 * boards is the ordinary case, on NMDC it is the only case, and an
		 * interface that offered the user a board on such a hub is the thing that
		 * was wrong. Reporting it as a system error would put a message on screen
		 * about a hub that behaved perfectly.
		 *
		 * Every name here is protocol text and is checked before anything goes out;
		 * see the ADC implementation. A board is named rather than held, because a
		 * board belongs to one hub and the caller may know of several.
		 */

		/**
		 * Subscribe to a board, replaying it from 'timestamp' onwards.
		 *
		 * The backlog and the live feed are one stream and are not distinguished:
		 * every entry the hub holds from that second onwards arrives, and so does
		 * every entry it accepts afterwards, until the subscription is cancelled or
		 * the connection ends. Nothing marks the end of the backlog, because there
		 * is no end to it.
		 *
		 * @param timestamp the highest timestamp already held, and deliberately not
		 *        one second past it: timestamps are not unique, so a cursor moved
		 *        beyond the last entry received skips every post the hub accepted
		 *        in that same second. The price is that the final second arrives a
		 *        second time, which a caller keyed on the tth discards. Zero asks
		 *        for the whole board.
		 */
		virtual bool subscribeBoard(const char* board, uint64_t timestamp)
		{ (void) board; (void) timestamp; return false; }

		/**
		 * Cancel a subscription. Nothing confirms it, so a caller that wants to
		 * know it has stopped receiving a board has only the absence of entries.
		 */
		virtual bool unsubscribeBoard(const char* board)
		{ (void) board; return false; }

		/**
		 * Ask for the single index entry of one post, without subscribing.
		 *
		 * What an orphaned reply is chased with: the parent named by a post already
		 * held may be on a board this session cannot see, withdrawn, or from
		 * another hub, and this is the question that tells the three apart. The
		 * answer is one entry, or a refusal saying the hub holds no such post.
		 */
		virtual bool requestEntry(const char* board, const char* tth)
		{ (void) board; (void) tth; return false; }

		/**
		 * Submit a post that has already been composed, hashed and made servable.
		 *
		 * The caller is the only source of the document until the first reader has
		 * fetched it, so it must be prepared to serve it before this is called: an
		 * author who submits a post and disconnects has published nothing.
		 *
		 * There is no success message. The hub sends the resulting index entry back
		 * - to this session whether or not it subscribes to the board - and that
		 * entry is the acknowledgement, so nothing may be shown as published until
		 * it arrives.
		 *
		 * @param size  the document's length in bytes, which the hub weighs against
		 *              the board's limit.
		 * @param parent the tth of the post being replied to, or null for a post
		 *              that starts a thread. The thread root is not passed, because
		 *              it is not the client's to work out.
		 */
		virtual bool submitPost(const char* tth, uint64_t size, const char* board,
		                        const char* subject, const char* parent)
		{
			(void) tth; (void) size; (void) board; (void) subject; (void) parent;
			return false;
		}

		/**
		 * Withdraw a post from a board.
		 *
		 * Withdrawal is not deletion and the word is chosen for that: it removes an
		 * index entry from one hub, while the document is held by every client that
		 * read it and goes on being served by them. What arrives is a tombstone
		 * carrying a new timestamp, which is what puts it in front of every cursor
		 * that had already moved past the post.
		 */
		virtual bool withdrawPost(const char* tth, const char* board)
		{ (void) tth; (void) board; return false; }

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

