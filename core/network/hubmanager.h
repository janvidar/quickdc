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

namespace Share {
	class SearchRequest;
}

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
		 * Returns a hub where the given client ID is online, or 0.
		 */
		Hub* lookupUserByClientID(const char* cid);

		/**
		 * Returns the hub we are logged in to at the given URL, or 0.
		 * The URL must be in the canonical form returned by Hub::getUrl(),
		 * which is what a FileSource stores.
		 */
		Hub* getHubByUrl(const char* url);

		/** Returns a hub where the given nickname is online, or 0. */
		Hub* lookupUserByNick(const char* nick);

		/** Send a search to every hub we are logged in to. */
		size_t searchAllHubs(Share::SearchRequest* request);

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
