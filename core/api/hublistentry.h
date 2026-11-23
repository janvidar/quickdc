/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_HUBLISTENTRY_H
#define HAVE_QUICKDC_API_HUBLISTENTRY_H

#include <cstdint>
#include <string>

namespace QuickDC {

/**
 * One hub, as a hublist describes it.
 *
 * Every field but the address is decoration: the address is what a hub is, and
 * an entry without one names nothing. The numbers are what the list claims and
 * are not checked against anything - only the hub itself can say how many users
 * it has.
 */
struct HubListEntry
{
	std::string name;
	std::string address;
	std::string description;
	std::string country;
	uint64_t users = 0;
	uint64_t shared = 0;
	uint64_t minshare = 0;
	uint64_t minslots = 0;
	uint64_t maxhubs = 0;
	uint64_t maxusers = 0;
};

}

#endif // HAVE_QUICKDC_API_HUBLISTENTRY_H
