/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "network/dc/tag.h"


DC::Tag::Tag(const char* string) : ok(false) {
	parse(string ? string : "");
}

/*virtual*/
DC::Tag::~Tag() {
}

bool DC::Tag::isOK() { return ok; }

/**
 * "<client V:1.0,M:A,H:1/0/0,S:2>", out of another user's $MyINFO - so the shape
 * is whatever they sent, and anything that does not fit leaves the tag not ok.
 */
void DC::Tag::parse(std::string_view tag) {
	/* The two brackets, and something between them. */
	if (tag.size() < 2) return;
	tag.remove_prefix(1);
	tag.remove_suffix(1);

	const size_t space = tag.find(' ');
	if (space == std::string_view::npos) return;

	client = tag.substr(0, space);
	tag.remove_prefix(space + 1);

	/*
	 * "name:value" separated by commas. A name we do not know is skipped rather
	 * than refused: the tag is a convention, and clients add fields to it.
	 */
	while (!tag.empty()) {
		const size_t colon = tag.find(':');
		if (colon == std::string_view::npos) break;

		const std::string_view name = tag.substr(0, colon);
		tag.remove_prefix(colon + 1);

		const size_t comma = tag.find(',');
		if (comma == std::string_view::npos) {
			parsePair(name, tag);
			break;
		}

		parsePair(name, tag.substr(0, comma));
		tag.remove_prefix(comma + 1);
	}

	/* Process client name */
	if (client == "++")
		client = "DC++";

	ok = true;
}

bool DC::Tag::parsePair(std::string_view name, std::string_view value) {
	if (name.size() != 1 || value.empty()) return false;

	switch (name[0]) {
		case 'M': mode = value;      return true;
		case 'S': slotcount = value; return true;
		case 'H': hubcount = value;  return true;
		case 'V': version = value;   return true;
		default:                     return false;
	}
}

const char* DC::Tag::getUserAgent() {
	if (!ua.empty()) return ua.c_str();
	if (client.empty()) return 0;

	ua = client;
	if (!version.empty())
	{
		ua += ' ';
		ua += version;
	}
	return ua.c_str();
}

uint32_t DC::Tag::getSlots() {
	if (slotcount.empty()) return 0;
	int nslots = Samurai::Util::Convert::to_int32(slotcount.c_str());
	if (nslots < 0) return 0;
	return static_cast<uint32_t>(nslots);
}

/**
 * Format:
 * "num1/num2/num3"
 * num1 = hubs where user is an operator
 * num2 = hubs where user is registered
 * num3 = hubs where user is none of the above.
 */
bool DC::Tag::getHubCount(int& op, int& registered, int& regular) {
	if (hubcount.empty()) return false;

	const size_t first = hubcount.find('/');
	const size_t last  = hubcount.rfind('/');
	if (first == std::string::npos || first == last) return false;

	op         = Samurai::Util::Convert::to_int32(hubcount.substr(0, first).c_str());
	registered = Samurai::Util::Convert::to_int32(hubcount.substr(first + 1, last - first - 1).c_str());
	regular    = Samurai::Util::Convert::to_int32(hubcount.substr(last + 1).c_str());
	return true;
}

bool DC::Tag::isActiveMode() {
	return !mode.empty() && mode[0] == 'A';
}

bool DC::Tag::isPassiveMode() {
	return !mode.empty() && mode[0] == 'P';
}

bool DC::Tag::haveMode() {
	return isActiveMode() || isPassiveMode();
}
