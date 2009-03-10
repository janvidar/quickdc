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
		void sendPrivateChatMessage(const QuickDC::User*, const char*);
		void sendChatMessage(const char*);
		void sendAdminUserKick(const QuickDC::User*);
		void sendAdminUserRedirect(const QuickDC::User*, const char*, const char*);
		void sendClientInfo();
		void sendSearchRequest(QuickDC::Share::SearchRequest* request);
		void sendConnectionRequest(const QuickDC::User*, const char* token);
		bool knowUser(const char* username);
		const QuickDC::User* getUser(const char* username);
		const QuickDC::User* getLocalUser();
		QuickDC::UserManager* getUserManager();
		
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

		void onActiveConnect(const char* from, const char* protocol, uint16_t port, const char* token);
		void onPassiveConnect(const char* from, const char* protocol, const char* token);
		void onSearch(QuickDC::Share::SearchRequest* request);
		void onSearchResult(QuickDC::Share::SearchResult* result);
		
		void sendPeerError(const char* sid, const char* code, const char* msg, const char* token, const char* protocol);

	protected:
		void send(ADC::Command* cmd, bool more = false);
		void resetIdleTimer();
		
	public:
		virtual void EventHostLookup(const Samurai::IO::Net::Socket*);
		virtual void EventHostFound(const Samurai::IO::Net::Socket*);
		virtual void EventConnecting(const Samurai::IO::Net::Socket*);
		virtual void EventConnected(const Samurai::IO::Net::Socket*);
		virtual void EventTimeout(const Samurai::IO::Net::Socket*);
		virtual void EventDisconnected(const Samurai::IO::Net::Socket*);
		virtual void EventDataAvailable(const Samurai::IO::Net::Socket*);
		virtual void EventCanWrite(const Samurai::IO::Net::Socket*);
		virtual void EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char*);
		virtual void EventTLSConnected(const Samurai::IO::Net::Socket*);
		virtual void EventTLSDisconnected(const Samurai::IO::Net::Socket*);
		
		virtual void EventSearchReply(QuickDC::Share::SearchReply*);
		virtual void connect();
		virtual void disconnect();
		virtual bool isLoggedIn() const;
		
		/**
		 * Check if the server supports a given feature.
		 */
		bool supports(const char* feature);
		
	private:
		void EventUserJoin(const char* nick);
		void EventUserQuit(const QuickDC::User* nick, const char* msg);
		void EventUserInfo(const QuickDC::User* data);
		void EventTimeout(Samurai::Timer* timer);
		
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
		char* sid;
		char* cid;
		char* pid;
		char* hubname;
		char* hubdesc;
		char* hubversion;
		bool secure;

	public:
		QuickDC::UserManager* users;
		
	friend class QuickDC::HubManager;
	friend class QuickDC::Hub;
};

}

#endif // HAVE_QUICKDC_ADCHUBSESSION_H
