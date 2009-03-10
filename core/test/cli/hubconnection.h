/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "api/hub.h"


class HubConnection : public QuickDC::Hub, public QuickDC::HubListener {
	public:
		HubConnection(Samurai::IO::Net::URL* url);
		virtual ~HubConnection();
		
		void status(const char* string, const char* string2);

		void EventNetStatus(enum StatusNetwork netstate);
		void EventHubStatus(enum StatusHub);
		
		void EventChat(const QuickDC::User* user, const char* message, bool);
		
		void EventPrivateChat(const QuickDC::User* from, const QuickDC::User* to, const char* message, const char* context, bool action);
		void EventHubMessage(const char* user, const char* message);
		
		void EventUserJoin(const char* user);
		void EventUserLeave(const QuickDC::User* user, const char* message, bool);
		
		void EventUserUpdate(const QuickDC::User*);
		void EventUsersCleanup();
		void EventSearchResult(void*);
		void EventHubName(const char* hubname);
		bool EventHubRedirect(const char*, uint16_t);
		void EventHubAutenticate();
		
		bool EventClientConnect(const char* addr, uint16_t port, const QuickDC::User* u);
		
		bool EventClientConnect(const QuickDC::User* u);
		
	public:
		bool showjoins;
		bool allowConnections;
};
