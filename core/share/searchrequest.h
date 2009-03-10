/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SEARCHREQUEST_H
#define HAVE_QUICKDC_SEARCHREQUEST_H

#include "quickdc.h"
#include <vector>
#include "share/sharemanager.h"

namespace DC  { class HubSession; }
namespace ADC { class HubSession; }

namespace QuickDC {
class DCHubSession;
class User;
}

namespace QuickDC {
namespace Share {

class Dir;
class File;
class SearchReply;
class SearchRequest;

class SearchReplyHandler {
	public:
		SearchReplyHandler() { }
		virtual ~SearchReplyHandler() { }
	
	public:
		/**
		 * A search result is received from the search engine.
		 */
		virtual void EventSearchReply(SearchReply*) = 0;
};

/**
 * Locally generated search replies.
 */
class SearchReply {
	public:
		SearchReply(SearchRequest* request_, QuickDC::Share::File* file_) : request(request_), file(file_) { }
		virtual ~SearchReply() { }
		
		SearchRequest* getRequest() const { return request; }
		QuickDC::Share::File* getFile() const { return file; }
		
	protected:
		SearchRequest* request;
		QuickDC::Share::File* file;
};

/**
 * This class defines a search request as passed to the
 * search engine.
 */
class SearchRequest {

	public:
		enum SizePolicy { SizeAny, SizeMin, SizeMax, SizeExact };
		enum FilePolicy { SearchAll, SearchFiles, SearchDirectories };

		SearchRequest(SearchReplyHandler*);
		SearchRequest(enum SizePolicy size_type, uint64_t size, const char* tth, const char* token, SearchReplyHandler*);
		virtual ~SearchRequest();
		
		void addInclusion(const char* term);
		void addExclusion(const char* term);
		void addExtension(const char* ext);
		
		bool isTTH() const;
		char* getToken() const { return token; }
		char* getTTH() const { return tth; }
		uint64_t getSize() const { return size; }
		enum SizePolicy getSizePolicy() const { return size_type; }
		enum FilePolicy getFileType() const { return file_type; }

		int getMaxResults();

		void setUser(const QuickDC::User* user);
		const QuickDC::User* getUser() const;
		void setToken(const char*);
		void setTTH(const char*);
		void setSize(uint64_t size);
		void setSizePolicy(enum SizePolicy);
		void setType(enum FilePolicy);

	protected:
		std::vector<char*> include;
		std::vector<char*> exclude;
		std::vector<char*> extension;
		char* tth;
		char* token;
		uint64_t size;
		enum SizePolicy size_type;
		enum FilePolicy file_type;
		SearchReplyHandler* event_handler;
		const QuickDC::User* user;
		
	friend class QuickDC::Share::Manager;
	friend class ADC::HubSession;
	friend class DC::HubSession;
};

}
}


#endif // HAVE_QUICKDC_SEARCHREQUEST_H
