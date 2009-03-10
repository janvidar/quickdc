/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "network/dc/tag.h"


DC::Tag::Tag(const char* string) {
	ok = false;
	client = 0;
	version = 0;
	mode = 0;
	hubcount = 0;
	slotcount = 0;
	ua = 0;
	tag = strdup(string);
	parse();
}

/*virtual*/
DC::Tag::~Tag() {
	free(tag);
	free(client);
	free(version);
	free(mode);
	free(hubcount);
	free(slotcount);
	free(ua);
}

bool DC::Tag::isOK() { return ok; }

void DC::Tag::parse() {
	char* start = &tag[1];
	char* split = strchr(start, ' ');
	size_t len = strlen(tag);
	if (!split || !len) return;
	tag[len-1] = '\0';

	client = strndup(&start[0], &split[0]-&start[0]);
	start = &split[1];
	
	split = strchr(start, ':');
	while (split) {
		char* param_name = strndup(start, &split[0]-&start[0]);
		start = &split[1];
		char* param_data = 0;
		
		split = strchr(start, ',');
		if (split) {
			param_data = strndup(start, &split[0]-&start[0]);
			start = &split[1];
			split = strchr(start, ':');
		} else {
			param_data = strdup(start);
			start = &split[1];
			split = 0;
		}
		
		if (!parsePair(param_name, param_data)) {
			free(param_data);
		}
		free(param_name);
	}
	
	/* Process client name */
	if (strlen(client) == 2 && strncmp(client, "++", 2) == 0) {
		free(client);
		client = strdup("DC++");
	}
	ok = true;
}

#define BIND(c, member, value) { case c: free(member); member = value; }
bool DC::Tag::parsePair(char* name, char* value) {
	if (!name || !value || strlen(name) != 1) return false;

	switch (name[0]) {
		BIND('M', mode, value);      return true;
		BIND('S', slotcount, value); return true;
		BIND('H', hubcount, value);  return true;
		BIND('V', version, value);   return true;
		default:                     return false;
	}
}

const char* DC::Tag::getUserAgent() {
	if (ua) return ua;
	if (!client) return 0;
	if (!version) return client;
	ua = (char*) malloc(strlen(client) + strlen(version)+2);
	ua[0] = '\0';
	strcat(ua, client);
	strcat(ua, " ");
	strcat(ua, version);
	return ua;
}

uint32_t DC::Tag::getSlots() {
	if (!slotcount) return 0;
	int nslots = quickdc_atoi(slotcount);
	if (nslots < 0) return 0;
	return (uint32_t) nslots;
}

/**
 * Format:
 * "num1/num2/num3"
 * num1 = hubs where user is an operator
 * num2 = hubs where user is registered
 * num3 = hubs where user is none of the above.
 */
bool DC::Tag::getHubCount(int& op, int& registered, int& regular) {
	if (!hubcount) return false;
	char* tmp = strdup(hubcount);
	char* split1 = strchr(tmp, '/');
	char* split2 = strrchr(tmp, '/');
	if (!split1 || !split2 || split1 == split2) return false;
	split1[0] = '\0';
	split2[0] = '\0';
	op = quickdc_atoi(tmp);
	registered =  quickdc_atoi(&split1[1]);
	regular = quickdc_atoi(&split2[1]);
	free(tmp);
	return true;
}

bool DC::Tag::isActiveMode() {
	if (!mode) return false;
	return (mode[0] == 'A');
}

bool DC::Tag::isPassiveMode() {
	if (!mode) return false;
	return (mode[0] == 'P');
}

bool DC::Tag::haveMode() {
	if (!mode) return false;
	return (mode[0] == 'A' || mode[0] == 'P');
}

