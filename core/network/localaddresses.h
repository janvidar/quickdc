/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_LOCALADDRESSES_H
#define HAVE_QUICKDC_LOCALADDRESSES_H

#include <cstdint>
#include <string>
#include <vector>

namespace QuickDC {

/**
 * Whether the sixteen bytes of an IPv6 address could be reached from the internet:
 * not unspecified, loopback, link-local (fe80::/10), unique local (fc00::/7, what a
 * VPN overlay hands out), multicast, or an IPv4 address in disguise.
 */
bool isGlobalIPv6(const uint8_t address[16]);

/**
 * Whether any interface that is up holds a global IPv6 address.
 *
 * Without one there is no IPv6 route out, and a reachability check of that
 * family can only fail - so it is not worth asking a server for, every few
 * minutes, for the life of the process. Where the interfaces cannot be listed
 * the answer is yes, so that not knowing never stops a check.
 */
bool hostHasGlobalIPv6();

/**
 * The addresses of the interfaces that are up, less loopback and link-local,
 * sorted and joined: equal while nothing about where this host sits has
 * changed. Empty where the interfaces cannot be listed.
 */
std::string addressFingerprint();

/**
 * 'addresses' as addressFingerprint() compares them: loopback and link-local
 * dropped (on every machine, or what a host gives itself while DHCP decides),
 * duplicates folded, and sorted, since interfaces come back in no fixed order.
 */
std::string fingerprintOf(std::vector<std::string> addresses);

}

#endif // HAVE_QUICKDC_LOCALADDRESSES_H
