/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include "share/filetypes.h"
#include "share/searchrequest.h"
#include "network/dc/hubsession.h"
#include "ctype.h"
#include <string_view>

#define FREE_VECTOR(V) while (V.size()) { char* arg = V.back(); V.pop_back(); free(arg); }


QuickDC::Share::SearchRequest::SearchRequest(QuickDC::Share::SearchReplyHandler* eh) : size(0), size_type(SizeAny), file_type(SearchAll), event_handler(eh), user(0)
{

}

QuickDC::Share::SearchRequest::SearchRequest(enum SizePolicy sp, uint64_t size_, const char* tth_, const char* token_, QuickDC::Share::SearchReplyHandler* eh) :
	size(size_), size_type(sp), file_type(SearchAll), event_handler(eh), user(0)
{
	setTTH(tth_);
	setToken(token_);
}

QuickDC::Share::SearchRequest::~SearchRequest() {
	FREE_VECTOR(include);
	FREE_VECTOR(exclude);
	FREE_VECTOR(extension);
}

void QuickDC::Share::SearchRequest::setToken(const char* token_) {
	if (token_) token = token_;
}

void QuickDC::Share::SearchRequest::setTTH(const char* tth_) {
	const std::string_view root(tth_ ? tth_ : "");

	/* A TTH root is always 39 base32 characters; anything else is not one and
	   leaves the request without a TTH at all, which is what isTTH() answers. */
	tth.clear();
	if (root.size() != 39) return;

	tth = root;
	for (char& c : tth)
		c = (char) toupper((unsigned char) c);
}

void QuickDC::Share::SearchRequest::setSizePolicy(enum SizePolicy sp) { size_type = sp; }
void QuickDC::Share::SearchRequest::setType(enum FilePolicy fp) { file_type = fp; }


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
	return !tth.empty();
}

void QuickDC::Share::SearchRequest::setSize(uint64_t size_) {
	size = size_;
}


int QuickDC::Share::SearchRequest::getMaxResults() {
	return 10;
}

void QuickDC::Share::SearchRequest::setUser(const QuickDC::User* user_) { user = user_; }

const QuickDC::User* QuickDC::Share::SearchRequest::getUser() const { return user; }

