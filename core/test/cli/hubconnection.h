/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "api/hub.h"


class HubConnection : public QuickDC::Hub, public QuickDC::HubListener {
	public:
		HubConnection(Samurai::IO::Net::URL* url);
		virtual ~HubConnection();
		
		void status(const char* string, const char* string2);

		void EventNetStatus(enum StatusNetwork netstate) override;
		void EventHubStatus(enum StatusHub) override;
		void EventSystemError(const char*) override;

		void EventChat(const QuickDC::User* user, const char* message, bool, bool,
		               uint64_t) override;
		
		void EventPrivateChat(const QuickDC::User* from, const QuickDC::User* to, const char* message, const char* context, bool action, bool richText, uint64_t) override;
		void EventHubMessage(const char* user, const char* message, bool action,
		                     bool richText, uint64_t) override;
		
		void EventUserJoin(const char* user) override;
		void EventUserLeave(const QuickDC::User* user, const char* message, bool) override;

		void EventUserUpdate(const QuickDC::User*) override;
		void EventUsersCleanup() override;
		void EventSearchResult(void*) override;
		void EventHubName(const char* hubname) override;
		bool EventHubRedirect(const char*, uint16_t) override;
		void EventHubAutenticate() override;

		bool EventClientConnect(const char* addr, uint16_t port, const QuickDC::User* u) override;

		bool EventClientConnect(const QuickDC::User* u) override;

	public:
		bool showjoins;
		bool allowConnections;
};
