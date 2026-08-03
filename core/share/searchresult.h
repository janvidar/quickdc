/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SEARCHRESULT_H
#define HAVE_QUICKDC_SEARCHRESULT_H

#include <string>

namespace QuickDC {
class User;

namespace Share {

class SearchResult {
	public:
		SearchResult(const char* name, uint64_t size, size_t freeslots, const char* tth = 0, size_t tth_depth = 0, const char* token = 0);
		~SearchResult();

		void setUser(QuickDC::User* user);
		QuickDC::User* getUser() const;
		
		const char* getFile() const { return file.empty() ? 0 : file.c_str(); }
		uint64_t getSize() const { return size; }
		size_t getFreeSlots() const { return freeslots; }

		const char* getTTH() const { return tth.empty() ? 0 : tth.c_str(); }
		size_t getTTHDepth() const { return tth_depth; }

		const char* getToken() const { return token.empty() ? 0 : token.c_str(); }

	protected:
		/*
		 * A string left empty is a field the result did not carry, which is what
		 * its getter answers null for. None of the three has an empty value that
		 * means anything else.
		 */
		QuickDC::User* user;
		std::string file;
		uint64_t size;
		size_t freeslots;
		std::string tth;
		size_t tth_depth;
		std::string token;
};

}
}

#endif // HAVE_QUICKDC_SEARCHRESULT_H
