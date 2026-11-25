/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SEARCH_MANAGER_H
#define HAVE_QUICKDC_SEARCH_MANAGER_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

/*
 * How many searches are attributed to at once; the oldest is forgotten beyond
 * that, for a front end that never says it is done with one.
 */
#define QUICKDC_SEARCHES_LIVE 64

namespace QuickDC {
namespace Share {

class SearchResult;
class SearchRequest;

/**
 * The searches a front end sent and still wants answers to, and which of them a
 * result answers.
 *
 * By the token the result quotes back where it carries one. NMDC has nowhere to
 * put a token, so a result without one goes to the newest search whose words its
 * name holds, none of whose exclusions it does - a name matching two searches
 * says nothing to settle it, and the newest is the likelier to have been asked.
 */
class SearchManager {
	public:
		/** Remember 'request' as it went out; returns the number it goes by. */
		uint64_t add(const SearchRequest& request);

		/** No more results for 'search'; false where it was not live. */
		bool remove(uint64_t search);

		/** The live search 'result' answers, or 0 for none. */
		uint64_t attribute(const SearchResult& result) const;

		size_t live() const { return searches.size(); }

	private:
		struct Live {
			uint64_t id = 0;
			std::string token;
			std::vector<std::string> include;
			std::vector<std::string> exclude;
		};

		/* Oldest first. */
		std::deque<Live> searches;
		uint64_t last = 0;
};

}
}

#endif // HAVE_QUICKDC_SEARCH_MANAGER_H
