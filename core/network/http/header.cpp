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
		char* split = strchr(line, ':');
		if (split && strlen(split) > 1) {
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
	if (!name || !value) return false;
	for (size_t n = 0; n < strlen(name); n++) {
		
	}
	return true;
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
		if (!strcasecmp(header->getName(), name) && header->isValid()) cnt++;
	}
	return cnt;
}

Http::Header* Http::HeaderList::get(const char* name) {
	for (std::vector<Http::Header*>::iterator it = headers.begin(); it != headers.end(); it++) {
		Http::Header* header = (*it);
		if (!strcasecmp(header->getName(), name) && header->isValid()) return header;
	}
	return 0;
}


void Http::HeaderList::compose(Samurai::IO::Buffer* out) {
	for (std::vector<Http::Header*>::iterator it = headers.begin(); it != headers.end(); it++) {
		Http::Header* header = (*it);
		out->append(header->getName());
		out->append(": ");
		out->append(header->getValue());
		out->append("\r\n");
	}
}


