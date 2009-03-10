/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SEARCHRESULT_H
#define HAVE_QUICKDC_SEARCHRESULT_H

namespace QuickDC {
class User;

namespace Share {

class SearchResult {
	public:
		SearchResult(const char* name, uint64_t size, size_t freeslots, const char* tth = 0, size_t tth_depth = 0, const char* token = 0);
		~SearchResult();

		void setUser(QuickDC::User* user);
		QuickDC::User* getUser() const;
		
		char* getFile() const { return file; }
		uint64_t getSize() const { return size; }
		size_t getFreeSlots() const { return freeslots; }
		
		char* getTTH() const { return tth; }
		size_t getTTHDepth() const { return tth_depth; }
		
		char* getToken() const { return token; }

	protected:
		QuickDC::User* user;
		char* file;
		uint64_t size;
		size_t freeslots;
		char* tth;
		size_t tth_depth;
		char* token;
};

}
}

#endif // HAVE_QUICKDC_SEARCHRESULT_H
