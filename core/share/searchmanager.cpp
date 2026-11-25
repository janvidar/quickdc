/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "share/searchmanager.h"
#include "share/searchrequest.h"
#include "share/searchresult.h"

#include <samurai/stdc.h>

#include <algorithm>

uint64_t QuickDC::Share::SearchManager::add(const SearchRequest& request)
{
	Live search;
	search.id = ++last;
	if (const char* token = request.getToken()) search.token = token;
	search.include = request.getInclusions();
	search.exclude = request.getExclusions();
	searches.push_back(std::move(search));

	while (searches.size() > QUICKDC_SEARCHES_LIVE)
		searches.pop_front();

	return last;
}

bool QuickDC::Share::SearchManager::remove(uint64_t search)
{
	const auto found = std::find_if(searches.begin(), searches.end(),
	                                [search](const Live& live) { return live.id == search; });
	if (found == searches.end()) return false;

	searches.erase(found);
	return true;
}

uint64_t QuickDC::Share::SearchManager::attribute(const SearchResult& result) const
{
	if (const char* token = result.getToken())
	{
		for (const Live& search : searches)
			if (!search.token.empty() && search.token == token) return search.id;
		return 0;
	}

	const char* file = result.getFile();
	if (!file) return 0;
	const std::string_view name(file);

	for (auto search = searches.crbegin(); search != searches.crend(); ++search)
	{
		if (search->include.empty()) continue;

		const auto holds = [name](const std::string& word) { return Samurai::Util::icontains(name, word); };
		if (std::all_of(search->include.begin(), search->include.end(), holds)
		    && std::none_of(search->exclude.begin(), search->exclude.end(), holds))
		{
			return search->id;
		}
	}

	return 0;
}
