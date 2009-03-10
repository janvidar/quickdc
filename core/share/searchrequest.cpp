/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include "share/filetypes.h"
#include "share/searchrequest.h"
#include "network/dc/hubsession.h"
#include "ctype.h"

#define FREE_VECTOR(V) while (V.size()) { char* arg = V.back(); V.pop_back(); free(arg); }


QuickDC::Share::SearchRequest::SearchRequest(QuickDC::Share::SearchReplyHandler* eh) : tth(0), token(0), size(0), size_type(SizeAny), event_handler(eh), user(0)
{
	
}

QuickDC::Share::SearchRequest::SearchRequest(enum SizePolicy sp, uint64_t size_, const char* tth_, const char* token_, QuickDC::Share::SearchReplyHandler* eh) :
	tth(0), token(0), size(size_), size_type(sp), event_handler(eh), user(0)
{
	if (tth_) {
		if (strlen(tth_) == 39)
			tth = strdup(tth_);
			
		for (size_t n = 0; n < 39; n++)
			tth[n] = toupper(tth[n]);
	}
	if (token_) token = strdup(token_);
}

QuickDC::Share::SearchRequest::~SearchRequest() {
	free(tth);
	free(token);

	FREE_VECTOR(include);
	FREE_VECTOR(exclude);
	FREE_VECTOR(extension);
}

void QuickDC::Share::SearchRequest::setToken(const char* token_) {
	if (token_) {
		if (token) free(token);
		token = strdup(token_);
	}
}

void QuickDC::Share::SearchRequest::setTTH(const char* tth_) {
	if (tth_) {
		if (tth) free(tth);
		if (strlen(tth_) == 39) {
			tth = strdup(tth_);
			for (size_t n = 0; n < 39; n++)
				tth[n] = toupper(tth[n]);
		}
	}
}

void QuickDC::Share::SearchRequest::setSizePolicy(enum SizePolicy) { }
void QuickDC::Share::SearchRequest::setType(enum FilePolicy) { }


void QuickDC::Share::SearchRequest::addInclusion(const char* term) {
	if (term && strlen(term) >= 3) {
		include.push_back(strdup(term));
		QSEARCH("Search term: '%s'", term);
	}
}

void QuickDC::Share::SearchRequest::addExclusion(const char* term) {
	if (term && strlen(term) >= 3)
		exclude.push_back(strdup(term));
}

void QuickDC::Share::SearchRequest::addExtension(const char* ext) {
	if (ext)
		extension.push_back(strdup(ext));
}

bool QuickDC::Share::SearchRequest::isTTH() const {
	return tth != 0;
}

void QuickDC::Share::SearchRequest::setSize(uint64_t size_) {
	size = size_;
}


int QuickDC::Share::SearchRequest::getMaxResults() {
	return 10;
}

void QuickDC::Share::SearchRequest::setUser(const QuickDC::User* user_) { user = user_; }

const QuickDC::User* QuickDC::Share::SearchRequest::getUser() const { return user; }

