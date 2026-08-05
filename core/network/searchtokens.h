/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SEARCHTOKENS_H
#define HAVE_QUICKDC_SEARCHTOKENS_H

#include "quickdc.h"

#include <deque>
#include <string>

/*
 * How many searches are remembered.
 *
 * A reply arrives seconds after the search that asked for it, so only the recent
 * ones can be answered - and the set is bounded because a client searching all
 * evening would otherwise remember every search it ever made.
 */
#define QUICKDC_SEARCH_TOKENS_REMEMBERED 32

namespace QuickDC {

/**
 * The searches this client sent, so that a reply can be told from a stranger.
 *
 * A datagram's source address is whatever its sender wrote, so an unsolicited one
 * says nothing about where it came from and cannot be evidence of anything. One
 * carrying a token from a search this client sent is at least an answer to
 * something, which is the most a datagram can be held to.
 *
 * What a token does not establish is who sent it. A search goes to every client on
 * the hub, so the token in it is public the moment it is sent - anyone who saw it
 * can quote it back. It bounds the evidence to "this answers something we asked",
 * not to "this came from the peer we expected".
 */
class SearchTokens
{
	public:
		/**
		 * A fresh token, remembered as outstanding.
		 *
		 * @return the token, or an empty string if one could not be drawn - in
		 *         which case the search should go out without one rather than with
		 *         something guessable.
		 */
		std::string issue();

		/**
		 * Remember a token a search is going out with, whoever drew it.
		 *
		 * The queue draws its own for a source search, so not every search's token
		 * comes from issue() - and one that is not remembered is one whose answer
		 * cannot be told from a stranger's.
		 */
		void remember(const char* token);

		/** Whether this is a token from a search still remembered. */
		bool issued(const char* token) const;

		size_t outstanding() const { return tokens.size(); }

		void forget();

	private:
		/* In the order they were issued, so the oldest is the one to drop. */
		std::deque<std::string> tokens;
};

}

#endif // HAVE_QUICKDC_SEARCHTOKENS_H
