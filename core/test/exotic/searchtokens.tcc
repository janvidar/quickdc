/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The searches this client sent, remembered so that a reply can be told from a
 * stranger.
 *
 * This exists because a datagram's source address is whatever its sender wrote. An
 * unsolicited datagram therefore says nothing about where it came from, and the only
 * thing that can be asked of one is what it answers - so a search goes out naming
 * itself, and the names are kept here.
 *
 * What a token does not establish is who sent it: a search reaches every client on
 * the hub, so its token is public the moment it is sent. These cases are about the
 * one thing it does establish.
 */

#include "quickdc.h"
#include "network/searchtokens.h"

#include <samurai/util/base32.h>

#include <set>
#include <string>

/* ------------------------------------------------------------------------- */
/* Issuing                                                                   */
/* ------------------------------------------------------------------------- */

EXO_TEST(searchtokens_a_fresh_token_is_remembered,
{
	QuickDC::SearchTokens tokens;

	const std::string token = tokens.issue();
	EXO_ASSERT(!token.empty());
	EXO_ASSERT(tokens.issued(token.c_str()));
	EXO_ASSERT_EQ_UINT(tokens.outstanding(), 1u);
	return 1;
});

/* Nothing else is. */
EXO_TEST(searchtokens_a_token_never_issued_is_not_recognised,
{
	QuickDC::SearchTokens tokens;
	tokens.issue();

	EXO_ASSERT(!tokens.issued("SOMETHINGELSE"));
	EXO_ASSERT(!tokens.issued(""));
	EXO_ASSERT(!tokens.issued(0));
	return 1;
});

/* An empty record recognises nothing at all. */
EXO_TEST(searchtokens_nothing_is_recognised_before_anything_is_sent,
{
	QuickDC::SearchTokens tokens;

	EXO_ASSERT(!tokens.issued("ANYTHING"));
	EXO_ASSERT_EQ_UINT(tokens.outstanding(), 0u);
	return 1;
});

/*
 * Drawn rather than counted. A token from a counter is one a stranger who never saw
 * the search could produce, which is the whole thing this is meant to tell apart.
 */
EXO_TEST(searchtokens_a_token_is_not_the_same_twice,
{
	QuickDC::SearchTokens tokens;

	std::set<std::string> seen;
	for (int n = 0; n < QUICKDC_SEARCH_TOKENS_REMEMBERED; n++)
	{
		const std::string token = tokens.issue();
		EXO_ASSERT(!token.empty());

		if (!seen.insert(token).second)
		{
			const std::string why = "the token \"" + token + "\" was issued twice";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/* And every position of it varies, rather than a counter moving the tail. */
EXO_TEST(searchtokens_a_token_varies_across_its_whole_length,
{
	QuickDC::SearchTokens tokens;

	std::vector<std::string> issued;
	for (int n = 0; n < QUICKDC_SEARCH_TOKENS_REMEMBERED; n++)
		issued.push_back(tokens.issue());

	const size_t length = issued[0].size();
	EXO_ASSERT(length >= 8);

	for (size_t at = 0; at < length; at++)
	{
		std::set<char> here;
		for (const std::string& token : issued)
		{
			EXO_ASSERT_EQ_UINT(token.size(), length);
			here.insert(token[at]);
		}

		if (here.size() < 2)
		{
			const std::string why = "position " + std::to_string(at)
				+ " is the same character in every token";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/* Base32, because it goes on a protocol line as an argument. */
EXO_TEST(searchtokens_a_token_needs_no_escaping_on_a_protocol_line,
{
	QuickDC::SearchTokens tokens;

	for (int n = 0; n < 16; n++)
	{
		const std::string token = tokens.issue();
		for (char c : token)
		{
			const bool base32 = (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
			if (!base32)
			{
				const std::string why = "the token \"" + token
					+ "\" contains a character outside base32";
				EXO_FAIL(why.c_str());
			}
		}
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Remembering one somebody else drew                                        */
/* ------------------------------------------------------------------------- */

/*
 * The queue draws its own token for a source search, so not every search's token
 * comes from issue() - and one that is not remembered is one whose answer cannot be
 * told from a stranger's.
 */
EXO_TEST(searchtokens_a_token_drawn_elsewhere_can_be_remembered,
{
	QuickDC::SearchTokens tokens;

	tokens.remember("srcABCDEFGH");
	EXO_ASSERT(tokens.issued("srcABCDEFGH"));
	EXO_ASSERT_EQ_UINT(tokens.outstanding(), 1u);
	return 1;
});

/* The same search asked at several hubs is one search. */
EXO_TEST(searchtokens_remembering_one_twice_records_it_once,
{
	QuickDC::SearchTokens tokens;

	for (int n = 0; n < 10; n++) tokens.remember("srcABCDEFGH");

	EXO_ASSERT_EQ_UINT(tokens.outstanding(), 1u);
	EXO_ASSERT(tokens.issued("srcABCDEFGH"));
	return 1;
});

/* Nothing is not a token. */
EXO_TEST(searchtokens_remembering_nothing_records_nothing,
{
	QuickDC::SearchTokens tokens;

	tokens.remember(0);
	tokens.remember("");

	EXO_ASSERT_EQ_UINT(tokens.outstanding(), 0u);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Forgetting the old ones                                                   */
/* ------------------------------------------------------------------------- */

/*
 * A client searching all evening would otherwise remember every search it ever made.
 * A reply arrives seconds after the search that asked for it, so the ones that fall
 * off the end are ones nothing will answer.
 */
EXO_TEST(searchtokens_only_the_recent_searches_are_remembered,
{
	QuickDC::SearchTokens tokens;

	for (int n = 0; n < QUICKDC_SEARCH_TOKENS_REMEMBERED * 4; n++)
	{
		tokens.remember(("TOKEN" + std::to_string(n)).c_str());

		if (tokens.outstanding() > QUICKDC_SEARCH_TOKENS_REMEMBERED)
		{
			const std::string why = "the record grew to "
				+ std::to_string(tokens.outstanding());
			EXO_FAIL(why.c_str());
		}
	}

	EXO_ASSERT_EQ_UINT(tokens.outstanding(),
		(size_t) QUICKDC_SEARCH_TOKENS_REMEMBERED);
	return 1;
});

/* And it is the oldest that goes, not the newest. */
EXO_TEST(searchtokens_the_oldest_search_is_the_one_forgotten,
{
	QuickDC::SearchTokens tokens;

	tokens.remember("FIRST");
	for (int n = 0; n < QUICKDC_SEARCH_TOKENS_REMEMBERED; n++)
		tokens.remember(("TOKEN" + std::to_string(n)).c_str());

	EXO_ASSERT(!tokens.issued("FIRST"));

	/* The newest is still there, which is the one a reply is most likely for. */
	const std::string last = "TOKEN"
		+ std::to_string(QUICKDC_SEARCH_TOKENS_REMEMBERED - 1);
	EXO_ASSERT(tokens.issued(last.c_str()));
	return 1;
});

EXO_TEST(searchtokens_forgetting_leaves_nothing_behind,
{
	QuickDC::SearchTokens tokens;

	const std::string token = tokens.issue();
	tokens.remember("srcABCDEFGH");

	tokens.forget();

	EXO_ASSERT_EQ_UINT(tokens.outstanding(), 0u);
	EXO_ASSERT(!tokens.issued(token.c_str()));
	EXO_ASSERT(!tokens.issued("srcABCDEFGH"));
	return 1;
});
