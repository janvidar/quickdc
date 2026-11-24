/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/localaddresses.h"

#include <cstring>

#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

bool QuickDC::isGlobalIPv6(const uint8_t a[16])
{
	static const uint8_t zero[16] = {0};
	static const uint8_t loopback[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
	static const uint8_t mapped[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};

	if (!std::memcmp(a, zero, 16) || !std::memcmp(a, loopback, 16)) return false;
	if (!std::memcmp(a, mapped, 12)) return false;
	if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return false;   /* fe80::/10 */
	if ((a[0] & 0xfe) == 0xfc) return false;                   /* fc00::/7 */
	if (a[0] == 0xff) return false;                            /* multicast */
	return true;
}

bool QuickDC::hostHasGlobalIPv6()
{
#ifdef _WIN32
	return true;
#else
	ifaddrs* list = nullptr;
	if (getifaddrs(&list) != 0) return true;

	bool found = false;
	for (const ifaddrs* one = list; one && !found; one = one->ifa_next)
	{
		if (!one->ifa_addr || one->ifa_addr->sa_family != AF_INET6) continue;
		if (!(one->ifa_flags & IFF_UP)) continue;

		const auto* address = reinterpret_cast<const sockaddr_in6*>(one->ifa_addr);
		found = isGlobalIPv6(address->sin6_addr.s6_addr);
	}

	freeifaddrs(list);
	return found;
#endif
}
