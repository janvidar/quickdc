/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HUB_MANAGER_H
#define HAVE_QUICKDC_HUB_MANAGER_H

#include <vector>

namespace QuickDC {

class Socket;
class Hub;

/**
 * This class manages multiple hub connections.
 */
class HubManager {
	public:
		HubManager();
		virtual ~HubManager();
	
		void add(Hub* hub);
		void remove(Hub* hub);
		
		/**
		 * Returns a hub for a given user ID.
		 * @param id is either nickname (NMDC) or sid (ADC).
		 */
		Hub* lookupUser(const char* id);
		
		/**
		 * Returns the number of hubs where we are logged in
		 * as normal users.
		 */
		size_t countHubsNormal();
		
		/**
		 * Returns the number of hubs where we are logged in
		 * as registered users.
		 */
		size_t countHubsRegistered();
		
		/**
		 * Returns the number of hubs where we are logged in
		 * as operators.
		 */
		size_t countHubsOperator();
		
	protected:
		std::vector<Hub*> hubs;
};

}

#endif // HAVE_QUICKDC_HUB_MANAGER_H
