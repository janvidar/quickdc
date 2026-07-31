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
		void sendPrivateChatMessage(const QuickDC::User*, const char*) override;
		void sendChatMessage(const char*) override;
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
		
		void onHubName(char* txt);
		void onHubDesc(char* txt);
		void onHubVersion(char* txt);

		void onUserInfo(const char* id, QuickDC::User* data, uint64_t oldShare, bool changed);
		void onStatus(ADC::Command* cmd);

		void onPublicChat(const char* from, const char* message, bool action);
		void onPrivateChat(const char* to, const char* from, const char* message, const char* contextID, bool action);
		void onRequestPasword(char* token);
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
		
		/**
		 * Check if the server supports a given feature.
		 */
		bool supports(const char* feature);

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

	private:
		void EventUserJoin(const char* nick) override;
		void EventUserQuit(const QuickDC::User* nick, const char* msg) override;
		void EventUserInfo(const QuickDC::User* data) override;
		void EventTimeout(Samurai::Timer* timer) override;
		
	protected:
		std::set<char*> extensions;
		QuickDC::Preferences* session;
		char* address;
		
		ADC::CommandProxy* proxy;
		QuickDC::HubListener* listener;
		Samurai::Timer* infoTimer;
		enum UserState userState;
		
	protected:
		char* nickname;
		char* description;
		char* email;
		char* password;

		/* The token the hub sent with its request. The answer is derived from
		   it, so it has to outlive the request while a password is found. */
		char* passwordToken;
		char* sid;
		char* cid;
		char* pid;
		char* hubname;
		char* hubdesc;
		char* hubversion;
		bool secure;

		/** The keyprint from the 'kp' parameter of the hub address, if any. */
		char* expectedKeyprint;

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
