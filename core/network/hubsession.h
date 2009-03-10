/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HUBSESSION_H
#define HAVE_QUICKDC_HUBSESSION_H


namespace QuickDC {
class Hub;
class User;
class UserManager;

namespace Share {
	class SearchRequest;
}

/**
 * A HubSession is the API for the hub.
 * This is subclassed as DCHubSession and ADCHubSession.
 */
class HubSession {
	public:
		HubSession(Hub* ui) : hub(ui), reconnect_time(-1) { }
		virtual ~HubSession() { }

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
		virtual void sendPrivateChatMessage(const User* user, const char* message) = 0;

		/**
		 * Send a public chat message to all users.
		 */
		virtual void sendChatMessage(const char* message) = 0;

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
		 * Reconnect in this amount of seconds,
		 * or -1 if never reconnect.
		 */
		virtual int getReconnectTime() const { return reconnect_time; }
		
		
		/**
		 * Returns true if the connection is established and we have an idle
		 * successfully logged in session on the hub.
		 */
		 virtual bool isLoggedIn() const = 0;
		
		
	protected:
		enum UserAccessLevel access;
		Hub* hub;
		int reconnect_time;
		
	friend class HubManager;
};

}

#endif // HAVE_QUICKDC_HUBSESSION_H

