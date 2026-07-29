/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdlib.h>
#include <string.h>
#include "network/http/header.h"

Http::Header::Header(const char* line) {
	name = 0;
	value = 0;
	if (line && strlen(line)) {
		const char* split = strchr(line, ':');

		/* A field value is allowed to be empty, so anything with a name and a
		   colon is a header; only the name has to be there. */
		if (split && split != line) {
			name = strndup(line, &split[0]-&line[0]);

			size_t offset = 1;
			for (; split[offset] == ' '; offset++) { }
			value = strdup(&split[offset]);
		}
	}

	QDBG("Http::Header::Header(): {%s} - {%s}", name, value);
}

Http::Header::Header(const char* name_, const char* value_) {
	name = strdup(name_);
	value = strdup(value_);
}

Http::Header::~Header() {
	free(name);
	free(value);
}

bool Http::Header::isValid() const {
	return name && value;
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
	size_t cnt = 0;
	for (std::vector<Http::Header*>::iterator it = headers.begin(); it != headers.end(); it++) {
		Http::Header* header = (*it);
		if (header->isValid() && !strcasecmp(header->getName(), name)) cnt++;
	}
	return cnt;
}

Http::Header* Http::HeaderList::get(const char* name) {
	for (std::vector<Http::Header*>::iterator it = headers.begin(); it != headers.end(); it++) {
		Http::Header* header = (*it);
		if (header->isValid() && !strcasecmp(header->getName(), name)) return header;
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


