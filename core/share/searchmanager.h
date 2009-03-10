/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SEARCH_MANAGER_H
#define HAVE_QUICKDC_SEARCH_MANAGER_H

#include <vector>
#include <samurai/timestamp.h>

namespace Samurai {
namespace IO {
class File;
}
}

namespace QuickDC {
namespace Share {

class SearchResult;
class SearchRequest;

class SearchResultHandler
{
	public:
		virtual ~SearchResultHandler() { }
		
		/**
		 * Implement this to be able to receive search results.
		 */
		virtual void EventSearchResult(Share::SearchResult*) = 0;
};

/**
 * This class manages active search requests, and 
 * will sort incoming search results to the correct 
 */
class SearchManager {
	public:
		SearchManager(Samurai::IO::File* file);
		virtual ~SearchManager();
	
		/*
		void addResultHandler(Share::SearchResultHandler* resultHandler);
		void removeResultHandler(Share::SearchResultHandler* resultHandler);
		*/
		
		void add(Share::SearchRequest* request, Share::SearchResultHandler* resultHandler = 0);
		void remove(Share::SearchRequest* request);
		
		/**
		 * Send the search result to the correct search result handler.
		 * If no handler is found, drop it.
		 * 
		 * NOTE: some searches don't have have a token (only NMDC), 
		 *       in that case we will have to match based on name or tth root.
		 */
		void handle(Share::SearchResult* result);

	protected:
		/**
		 * Load active searches from file.
		 */
		bool load();
		
		/**
		 * Save active searches to file.
		 */
		bool save();
		
	protected:
		
		struct SearchContainer {
			QuickDC::Share::SearchRequest* request;        /* The request */
			QuickDC::Share::SearchResultHandler* handler;  /* Who will handle the result? */
			Samurai::TimeStamp timestamp;                  /* When was the search request last sent? */
		};
		
		std::vector<SearchContainer*> active_searches;
		Samurai::IO::File* file;
};

}
}

#endif // HAVE_QUICKDC_SEARCH_MANAGER_H
