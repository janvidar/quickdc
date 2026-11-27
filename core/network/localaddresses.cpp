/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/localaddresses.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <samurai/io/net/socketglue.h>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
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

std::string QuickDC::fingerprintOf(std::vector<std::string> addresses)
{
	const auto ignored = [](const std::string& text) {
		Samurai::IO::Net::winsock_ready();
		in_addr four {};
		if (inet_pton(AF_INET, text.c_str(), &four) == 1)
		{
			const uint32_t host = ntohl(four.s_addr);
			return (host >> 24) == 127 || (host >> 16) == 0xa9fe;   /* 127/8, 169.254/16 */
		}
		in6_addr six {};
		if (inet_pton(AF_INET6, text.c_str(), &six) == 1)
		{
			const uint8_t* a = six.s6_addr;
			static const uint8_t loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
			return std::memcmp(a, loopback, 16) == 0 || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80);
		}
		return text.empty();
	};

	addresses.erase(std::remove_if(addresses.begin(), addresses.end(), ignored), addresses.end());
	std::sort(addresses.begin(), addresses.end());
	addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());

	std::string out;
	for (const std::string& one : addresses) out += (out.empty() ? "" : " ") + one;
	return out;
}

std::string QuickDC::addressFingerprint()
{
	std::vector<std::string> found;
	char text[INET6_ADDRSTRLEN] = {};

#ifdef _WIN32
	Samurai::IO::Net::winsock_ready();

	ULONG size = 16 * 1024;
	std::vector<unsigned char> buffer;
	ULONG result = ERROR_BUFFER_OVERFLOW;
	for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; attempt++)
	{
		buffer.resize(size);
		result = GetAdaptersAddresses(AF_UNSPEC,
			GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
	}
	if (result != NO_ERROR) return std::string();

	for (const IP_ADAPTER_ADDRESSES* one = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
	     one; one = one->Next)
	{
		if (one->OperStatus != IfOperStatusUp) continue;

		for (const IP_ADAPTER_UNICAST_ADDRESS* address = one->FirstUnicastAddress; address; address = address->Next)
		{
			const sockaddr* at = address->Address.lpSockaddr;
			if (!at) continue;
			if (at->sa_family == AF_INET)
				inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(at)->sin_addr, text, sizeof(text));
			else if (at->sa_family == AF_INET6)
				inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(at)->sin6_addr, text, sizeof(text));
			else continue;
			found.push_back(text);
		}
	}
#else
	ifaddrs* list = nullptr;
	if (getifaddrs(&list) != 0) return std::string();

	for (const ifaddrs* one = list; one; one = one->ifa_next)
	{
		if (!one->ifa_addr || !(one->ifa_flags & IFF_UP)) continue;

		const int family = one->ifa_addr->sa_family;
		if (family == AF_INET)
			inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(one->ifa_addr)->sin_addr, text, sizeof(text));
		else if (family == AF_INET6)
			inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(one->ifa_addr)->sin6_addr, text, sizeof(text));
		else continue;

		found.push_back(text);
	}
	freeifaddrs(list);
#endif

	return fingerprintOf(std::move(found));
}
