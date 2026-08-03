/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include "network/http/header.h"

Http::Header::Header(const char* line) {
	const std::string_view text(line ? line : "");
	const size_t split = text.find(':');

	/* A field value is allowed to be empty, so anything with a name and a
	   colon is a header; only the name has to be there. */
	if (split != std::string_view::npos && split != 0) {
		name = text.substr(0, split);

		std::string_view rest = text.substr(split + 1);
		while (!rest.empty() && rest.front() == ' ')
			rest.remove_prefix(1);
		value = std::string(rest);
	}

	QDBG("Http::Header::Header(): {%s} - {%s}", getName(), getValue());
}

Http::Header::Header(const char* name_, const char* value_)
	: name(name_), value(value_)
{
}

Http::Header::~Header() {
}

bool Http::Header::isValid() const {
	return !name.empty() && value.has_value();
}

Http::HeaderList::HeaderList() {

}

Http::HeaderList::~HeaderList() {
	while (headers.size()) {
		Http::Header* header = headers.back();
		headers.pop_back();
		delete header;
	}
}

void Http::HeaderList::add(Http::Header* header) {
	headers.push_back(header);
}

size_t Http::HeaderList::count(const char* name) {
	/* A valid header has a name, so getName() answers with one; the field being
	   looked for is the side that may be missing. */
	const std::string_view wanted(name ? name : "");

	size_t cnt = 0;
	for (std::vector<Http::Header*>::iterator it = headers.begin(); it != headers.end(); it++) {
		Http::Header* header = (*it);
		if (header->isValid() && Samurai::Util::iequals(header->getName(), wanted)) cnt++;
	}
	return cnt;
}

Http::Header* Http::HeaderList::get(const char* name) {
	const std::string_view wanted(name ? name : "");

	for (std::vector<Http::Header*>::iterator it = headers.begin(); it != headers.end(); it++) {
		Http::Header* header = (*it);
		if (header->isValid() && Samurai::Util::iequals(header->getName(), wanted)) return header;
	}
	return 0;
}


void Http::HeaderList::compose(Samurai::IO::Buffer* out) {
	for (std::vector<Http::Header*>::iterator it = headers.begin(); it != headers.end(); it++) {
		Http::Header* header = (*it);
		if (!header->isValid()) continue;
		out->append(header->getName());
		out->append(": ");
		out->append(header->getValue());
		out->append("\r\n");
	}
}


