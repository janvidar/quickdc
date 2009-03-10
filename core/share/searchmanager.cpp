/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "share/searchmanager.h"
#include "share/searchrequest.h"
#include "share/searchresult.h"
#include <samurai/io/file.h>

QuickDC::Share::SearchManager::SearchManager(Samurai::IO::File* file_)
{
	file = 0;
	if (file_)
		file = new Samurai::IO::File(*file_);

	load();
}

QuickDC::Share::SearchManager::~SearchManager()
{
	save();

	delete file;
}

bool QuickDC::Share::SearchManager::load()
{
	return false;	
}

bool QuickDC::Share::SearchManager::save()
{
	return false;
}

void QuickDC::Share::SearchManager::add(QuickDC::Share::SearchRequest* request, QuickDC::Share::SearchResultHandler* resultHandler)
{
	SearchContainer* container = new SearchContainer();
	container->request = request;
	container->handler = resultHandler;
	active_searches.push_back(container);
}

void QuickDC::Share::SearchManager::remove(QuickDC::Share::SearchRequest* request)
{
	if (!request || !active_searches.size()) return;
	
	std::vector<SearchContainer*>::iterator it = active_searches.begin();
	for (; it != active_searches.end(); it++)
	{
		SearchContainer* container = (*it);
		if (container->request == request)
		{
			active_searches.erase(it);
			break;
		}
	}
}

void QuickDC::Share::SearchManager::handle(QuickDC::Share::SearchResult* result)
{
	if (!result || !active_searches.size()) return;
	
	std::vector<SearchContainer*>::iterator it = active_searches.begin();
	char* token = result->getToken();
	if (token) {
		for (; it != active_searches.end(); it++) {
			SearchContainer* container = (*it);
			if (container->request->getToken()) {
				if (!strcmp(token, container->request->getToken())) {
					container->handler->EventSearchResult(result);
				}
			}
		}
	} else {
		/* FIXME: Ignoring token-less search for now. */
	}
}
