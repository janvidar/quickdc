/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/searchtokens.h"
#include "debug/dbg.h"

#include <samurai/util/base32.h>
#include <samurai/util/random.h>

#include <algorithm>

/*
 * How much entropy a search token carries.
 *
 * Not a secret - a search goes to every client on the hub, so the token in it is
 * public the moment it is sent. It is drawn rather than counted so that somebody
 * who never saw the search cannot produce one that looks like an answer to it.
 */
#define QUICKDC_SEARCH_TOKEN_ENTROPY 12

std::string QuickDC::SearchTokens::issue()
{
	unsigned char raw[QUICKDC_SEARCH_TOKEN_ENTROPY];
	if (!Samurai::Util::secureRandom(raw, sizeof(raw)))
	{
		/*
		 * No weaker substitute, for the same reason a transfer token has none: a
		 * token derived from the clock is one a stranger can produce, and a search
		 * carrying no token at all is honest about having nothing to check.
		 */
		QERR("Unable to obtain random bytes for a search token");
		return std::string();
	}

	char encoded[Samurai::Util::base32_encode_size(QUICKDC_SEARCH_TOKEN_ENTROPY)];
	Samurai::Util::base32_encode(raw, encoded);

	remember(encoded);
	return std::string(encoded);
}


void QuickDC::SearchTokens::remember(const char* token)
{
	if (!token || !*token) return;

	/* Already outstanding: a search repeated to several hubs is one search. */
	if (issued(token)) return;

	tokens.push_back(token);

	/* The oldest goes. A reply arrives seconds after the search that asked for it,
	   so the ones that have fallen off the end are ones nothing will answer. */
	while (tokens.size() > QUICKDC_SEARCH_TOKENS_REMEMBERED)
		tokens.pop_front();
}


bool QuickDC::SearchTokens::issued(const char* token) const
{
	if (!token || !*token) return false;

	return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}


void QuickDC::SearchTokens::forget()
{
	tokens.clear();
}
