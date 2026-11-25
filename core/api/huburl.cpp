/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/huburl.h"

std::string QuickDC::canonicalHubUrl(const std::string& url)
{
	const size_t first = url.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return std::string();
	std::string canonical = url.substr(first, url.find_last_not_of(" \t\r\n") - first + 1);

	size_t authority = canonical.find("://");
	if (authority == std::string::npos)
	{
		canonical.insert(0, "dchub://");
		authority = canonical.find("://");
	}
	authority += 3;

	size_t end = canonical.find_first_of("/?#", authority);
	if (end == std::string::npos) end = canonical.size();

	/* A colon inside brackets belongs to an IPv6 literal, so only one after the bracket is a port. */
	const std::string host = canonical.substr(authority, end - authority);
	const size_t colon = host.rfind(':');
	const size_t bracket = host.rfind(']');
	if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket))
		return canonical;

	canonical.insert(end, ":411");
	return canonical;
}
