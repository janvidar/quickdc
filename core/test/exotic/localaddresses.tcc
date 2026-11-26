/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Which IPv6 addresses mean this host has a route out on that family.
 *
 * An IPv6 reachability check is only worth asking for where one exists, and the
 * addresses a machine typically holds without one - its loopback, its link-local
 * address, a VPN overlay's unique-local one - must not count.
 */

#include "quickdc.h"
#include "network/localaddresses.h"

#include <samurai/io/net/socketglue.h>

#include <string>

namespace {

bool global(const char* text)
{
	uint8_t address[16];
	if (inet_pton(AF_INET6, text, address) != 1) return false;
	return QuickDC::isGlobalIPv6(address);
}

}

EXO_TEST(localaddresses_a_global_unicast_address_counts,
{
	EXO_ASSERT(global("2001:db8::1"));
	EXO_ASSERT(global("2a03:b0c0:3:f0:0:2:ce4c:7000"));
	return 1;
});

EXO_TEST(localaddresses_what_reaches_nowhere_does_not,
{
	EXO_ASSERT(!global("::"));
	EXO_ASSERT(!global("::1"));
	EXO_ASSERT(!global("fe80::1"));
	EXO_ASSERT(!global("febf::1"));
	EXO_ASSERT(!global("fd7a:115c:a1e0::1"));
	EXO_ASSERT(!global("fc00::1"));
	EXO_ASSERT(!global("ff02::1"));
	EXO_ASSERT(!global("::ffff:192.0.2.1"));
	return 1;
});

/* Whatever the answer on this machine, asking does not fail or hang. */
EXO_TEST(localaddresses_the_host_can_be_asked,
{
	(void) QuickDC::hostHasGlobalIPv6();
	return 1;
});
