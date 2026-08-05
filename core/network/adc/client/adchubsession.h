/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCHUBSESSION_H
#define HAVE_QUICKDC_ADCHUBSESSION_H

#include <string.h>
#include "network/connection.h"
#include "network/hubsession.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "share/searchrequest.h"
#include "config/preferences.h"
#include "api/hub.h"

#include <samurai/timer.h>
#include <optional>
#include <set>
#include <string>


namespace QuickDC {
class Hub;
class User;
class SearchRequest;
class HubSession;
class UserEventHandler;
class HubListener;
namespace Share {
class SearchResult;
class SearchRequest;
}
}


/*
 * The shortest salt this client will answer a password request with, in bytes
 * once decoded.
 *
 * The hub picks the salt and we hash the password with it, so a salt with nothing
 * in it makes the answer a function of the password alone - the same twenty-six
 * characters every time, for ever, replayable by anyone who ever saw one. A single
 * base32 character decodes to no bytes at all, which is exactly that case. 128
 * bits is the floor at which the answer is worth something.
 */
#define QUICKDC_ADC_SALT_MIN_BYTES 16

/*
 * Where the salt a hub last asked with is filed, against that hub.
 *
 * One value rather than a set: the exchange happens once per connection, so a hub
 * that reuses a salt does it on the next login rather than twice on this one.
 */
#define QUICKDC_ADC_SALT_SETTING "Last password salt"

namespace ADC {
class CommandProxy;
class Command;

/**
 * A class describing a current hub session.
 * In other words, a class holding all info related to a hub
 * connection.
 */
class HubSession :
		public QuickDC::HubSession,
		public QuickDC::UserEventHandler,
		public Samurai::TimerListener,
		public QuickDC::Connection,
		public QuickDC::Share::SearchReplyHandler
{
	public:
		HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener, Samurai::IO::Net::URL* url, bool secure);
		virtual ~HubSession();

		/* External API */
		void sendPrivateChatMessage(const QuickDC::User*, const char*,
		                            bool richText = false) override;
		void sendChatMessage(const char*, bool richText = false) override;
		void sendAdminUserKick(const QuickDC::User*) override;
		void sendAdminUserRedirect(const QuickDC::User*, const char*, const char*) override;
		void sendClientInfo() override;
		void sendSearchRequest(QuickDC::Share::SearchRequest* request) override;
		void sendConnectionRequest(const QuickDC::User*, const char* token) override;
		bool knowUser(const char* username);
		const QuickDC::User* getUser(const char* username) override;
		const QuickDC::User* getLocalUser() override;
		QuickDC::UserManager* getUserManager() override;
		
		// Events triggered by the server (hub)
		void onSID(char* sid);
		void addSupport(const char* txt);

		/** Called once the hub's entire SUP list has been read. */
		void onSupports();

		void onHubName(char* txt);
		void onHubDesc(char* txt);
		void onHubVersion(char* txt);

		void onUserInfo(const char* id, QuickDC::User* data, uint64_t oldShare, bool changed);
		void onStatus(ADC::Command* cmd);

		void onPublicChat(const char* from, const char* message, bool action,
		                  bool richText = false);
		void onPrivateChat(const char* to, const char* from, const char* message, const char* contextID, bool action, bool richText = false);
		void onRequestPasword(char* token);

		/**
		 * Whether a salt is one worth answering: long enough to make the answer
		 * depend on it, and not one this session has already answered.
		 *
		 * Reports through the listener when it is not, because the visible effect
		 * otherwise is only that the login does not complete.
		 */
		bool saltIsUsable(const std::string& salt);
		void sendPassword(const char* password) override;
		void sendPasswordResponse();

		void onActiveConnect(const char* from, const char* protocol, uint16_t port, const char* token);
		void onPassiveConnect(const char* from, const char* protocol, const char* token);
		void onSearch(QuickDC::Share::SearchRequest* request);
		void onSearchResult(QuickDC::Share::SearchResult* result) override;

		/** A peer told us which parts of a queued file it holds (PFSR). */
		void onPartialStatus(const char* cid, const char* tth, const char* indexes);

		/** Offer what we have of a queued file the searcher wanted. */
		void sendPartialStatus(QuickDC::Share::SearchRequest* request);
		
		void sendPeerError(const char* sid, const char* code, const char* msg, const char* token, const char* protocol) override;

	protected:
		/**
		 * Refuse a peer connection that would not be encrypted, telling both the
		 * peer and whoever is watching this session why.
		 */
		void refuseUnencrypted(const char* from, const char* token,
		                       const char* proto, const char* why);

		void send(ADC::Command* cmd, bool more = false);
		void resetIdleTimer();
		
	public:
		virtual void EventHostLookup(const Samurai::IO::Net::Socket*) override;
		virtual void EventHostFound(const Samurai::IO::Net::Socket*) override;
		virtual void EventConnecting(const Samurai::IO::Net::Socket*) override;
		virtual void EventConnected(const Samurai::IO::Net::Socket*) override;
		virtual void EventTimeout(const Samurai::IO::Net::Socket*) override;
		virtual void EventDisconnected(const Samurai::IO::Net::Socket*) override;
		virtual void EventDataAvailable(const Samurai::IO::Net::Socket*) override;
		virtual void EventCanWrite(const Samurai::IO::Net::Socket*) override;
		virtual void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char*) override;
		virtual void EventTLSConnected(const Samurai::IO::Net::Socket*) override;
		virtual void EventTLSDisconnected(const Samurai::IO::Net::Socket*) override;
		
		virtual void EventSearchReply(QuickDC::Share::SearchReply*) override;
		virtual void connect() override;
		virtual void disconnect() override;
		virtual bool isLoggedIn() const override;
		virtual bool isSecure() const override;
		virtual bool supportsRichText() const override;

		/**
		 * Check if the server supports a given feature.
		 */
		bool supports(const char* feature) const;

	protected:
		/**
		 * KEYP: compare the hub's certificate against the keyprint we expect,
		 * before a single byte of the session is sent.
		 *
		 * @return false if the connection was dropped, and the caller must
		 *         stop what it was doing.
		 */
		bool verifyKeyprint();

		/** Report a keyprint failure and drop the connection for good. */
		void reportKeyprintFailure(const char* msg);

		/** The session group a remembered keyprint is filed under. */
		const char* keyprintGroup() const;

		/**
		 * Our own SID as the command constructors take it, which is null until
		 * the hub has given us one. A command that would name us as its source
		 * is not written at all while that is the case.
		 */
		const char* localSid() const { return sid.empty() ? 0 : sid.c_str(); }

	private:
		void EventUserJoin(const char* nick) override;
		void EventUserQuit(const QuickDC::User* nick, const char* msg) override;
		void EventUserInfo(const QuickDC::User* data) override;
		void EventTimeout(Samurai::Timer* timer) override;
		
	protected:
		std::set<std::string> extensions;
		QuickDC::Preferences* session;
		std::string address;

		ADC::CommandProxy* proxy;
		QuickDC::HubListener* listener;
		Samurai::Timer* infoTimer;
		enum UserState userState;
		
	protected:
		/*
		 * All four come from the session or configuration file, so they are
		 * always a value and never an absence - an empty description or e-mail
		 * is a field the user left blank, and is sent as one.
		 */
		std::string nickname;
		std::string description;
		std::string email;
		std::string password;

		/* The token the hub sent with its request. The answer is derived from
		   it, so it has to outlive the request while a password is found. */
		std::string passwordToken;

		/*
		 * Our session ID, as the hub assigned it, and empty before it has. A
		 * SID is four base32 characters, so an empty one is no SID rather than
		 * a short one; the same goes for the hub's own name and version.
		 */
		std::string sid;

		std::string hubname;
		std::string hubdesc;
		std::string hubversion;
		bool secure;

		/**
		 * Where this hub says it sees us, and empty until it has said.
		 *
		 * Per hub rather than per client: the address a hub observes is the one it
		 * will hand to peers on that hub, and two hubs reached over different
		 * routes need not agree. Set from a status naming an address, which is a
		 * hub refusing what we claimed and saying what it saw instead.
		 */
		std::string observedAddress;

		/**
		 * The keyprint from the 'kp' parameter of the hub address, if any.
		 *
		 * Unset is the only thing that means "check nothing", which is why this
		 * is an optional rather than a string that happens to be empty: a
		 * keyprint that is there but unusable stops the connection instead, and
		 * the difference between the two is the whole of the authentication.
		 */
		std::optional<std::string> expectedKeyprint;

		/**
		 * Set once the hub has presented a certificate matching a keyprint we
		 * already had - either one that came with the hub address, or the one
		 * this hub used the last time we were here.
		 *
		 * This is what decides whether the KP fields the hub relays for other
		 * users can be believed: a hub connection nobody has vouched for could
		 * have had those fields rewritten en route.
		 */
		bool keyprintVerified;

	public:
		QuickDC::UserManager* users;
		
	friend class QuickDC::HubManager;
	friend class QuickDC::Hub;
};

}

#endif // HAVE_QUICKDC_ADCHUBSESSION_H
