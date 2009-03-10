/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "share/searchresult.h"
#include "network/user.h"

QuickDC::Share::SearchResult::SearchResult(const char* fn, uint64_t si, size_t sl, const char* tr, size_t td, const char* to) :
	user(0),
	file(0),
	size(si),
	freeslots(sl),
	tth(0),
	tth_depth(td),
	token(0)
{
	if (fn) file = strdup(fn);
	if (tr) tth = strdup(tr);
	if (to) token = strdup(to);
}

QuickDC::Share::SearchResult::~SearchResult() {
	free(file);
	free(tth);
	free(token);
}

void QuickDC::Share::SearchResult::setUser(QuickDC::User* u)
{
	user = u;
}

QuickDC::User* QuickDC::Share::SearchResult::getUser() const
{
	return user;
}

