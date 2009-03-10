/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCHUBSESSION_H
#define HAVE_QUICKDC_DCHUBSESSION_H

#include <string.h>
#include <set>
#include "api/hub.h"
#include "config/preferences.h"
#include "network/connection.h"
#include "network/user.h"
#include "network/hubsession.h"
#include "network/usermanager.h"
#include "share/searchrequest.h"
#include <samurai/timer.h>

namespace Samurai {
	class TimerListener;
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
}


namespace QuickDC {
	class Hub;
	class HubListener;
	class HubSession;
	class User;
	class UserEventHandler;
	namespace Share {
		class SearchRequest;
		class SearchResult;
	}
}

namespace DC {
class CommandProxy;
class Command;

/**
 * A class describing a current hub session.
 * In other words, a class holding all info related to a hub
 * connection.
 */
class HubSession
	: public QuickDC::HubSession
	, public QuickDC::UserEventHandler
	, public Samurai::TimerListener
	, public QuickDC::Connection
	, public QuickDC::Share::SearchReplyHandler
{
	public:
		enum State { None, Lock, Nick, Password, Idle };

		HubSession(QuickDC::Hub* ui, QuickDC::HubListener* listener, Samurai::IO::Net::URL* url);
		virtual ~HubSession();

		void sendPrivateChatMessage(const QuickDC::User* user, const char* msg);
		void sendChatMessage(const char* msg);
		void sendAdminUserKick(const QuickDC::User* user);
		void sendAdminUserRedirect(const QuickDC::User* user, const char* address, const char* reason);
		void sendClientInfo() { }
		void sendSearchRequest(QuickDC::Share::SearchRequest* request);
		void sendConnectionRequest(const QuickDC::User*, const char* token);
		
		// Events triggered by the server (hub)
		void onHubName(const char* hubname);
		void onLock(const char* lock, const char* pk);
		void onPublicChat(const char* from, const char* message);
		void onActionChat(const char* from, const char* message);
		void onPrivateChat(const char* to, const char* from, const char* message);
		void onRedirect(const char* host, uint16_t port);
		
		void onRequestMyInfo(const char* nick);
		void onRequestPasword();
		void onBadNick();
		void onBadPassword();
		void onLoggedIn();
		void onHubFull();
		void onNickList();
		void onOpList();
		void onSearch(QuickDC::Share::SearchRequest* request);
		void onSearchResult(QuickDC::Share::SearchResult* result);
		
		void onActiveConnect(const char* address, uint16_t port, const char* user);
		void onPassiveConnect(const char* nick);
		
		/**
		 * Add support for a feature a server supports
		 */
		void addLock(const char* lock, const char* key);
		void addSupport(const char* feature);
		bool supports(const char* feature);

		bool isExtendedProtocol() const { return extendedProtocol; }
		
		const char* getNick() const     { return nickname;    }
		const char* getPassword() const { return password;    }
		const char* getDescription()    { return description; }
		const char* getEmail() const    { return email;       }
		char getFlags() const            { return flags;       }
		// enum User::Access getAccess() const { return access; }
		void sendMyInfo();
		void sendPeerError(const char*, const char*, const char*, const char*, const char*) { } // not handled on NMDC
		
		bool knowUser(const char* username);
		const QuickDC::User* getUser(const char* username);
		const QuickDC::User* getLocalUser();
		QuickDC::UserManager* getUserManager();
		
		void connect();
		void disconnect();
		bool isLoggedIn() const;
		
		void send(DC::Command* cmd, bool more = false);
		
	protected:
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
		virtual void EventSearchReply(QuickDC::Share::SearchReply*);
		
	private:
		void EventUserJoin(const char* nick);
		void EventUserQuit(const QuickDC::User* data, const char* msg);
		void EventUserInfo(const QuickDC::User* data);
		void EventTimeout(Samurai::Timer* timer);
		
	protected:
		char* nickname;
		char* password;
		char* description;
		char* email;
		char* hubname;
		char* lock;
		char* key;
		char flags;
		bool extendedProtocol;
		
		std::set<char*> extensions;
		enum State state;
		QuickDC::Preferences* session;
		char* address;
		
		DC::CommandProxy* proxy;
		QuickDC::HubListener* listener;
		Samurai::Timer* infoTimer;
		
	public:
		QuickDC::UserManager* users;
		
	friend class QuickDC::HubManager;
	friend class QuickDC::Hub;
};

}

#endif // HAVE_QUICKDC_DCHUBSESSION_H
