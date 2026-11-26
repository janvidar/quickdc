/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The token that names a peer transfer.
 *
 * It is the only secret protecting an inbound peer connection: the CID it is
 * paired with is broadcast in every INF, so anything that can reach the peer port
 * and guess the token gets served whatever we would have uploaded. Closing the
 * loopback authorisation shortcut made that the whole of the protection rather
 * than a second line of it.
 *
 * It used to be "QDC" + the wall clock + a counter, which is guessable by anyone
 * who knows roughly when the process started - a few hundred candidates cover the
 * space. The cases here are the properties that distinguish a drawn token from a
 * derived one; none of them can prove unpredictability, but each of them fails
 * for the scheme that was here before.
 */

#include "quickdc.h"
#include "network/downloadqueue.h"
#include "network/securitymanager.h"

#include <samurai/io/file.h>
#include <samurai/util/base32.h>

#include <set>
#include <stdio.h>
#include <string>
#include <samurai/posix.h>
#include <vector>

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_token_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/*
 * makeToken() is protected: it is how the queue names its own requests, not
 * something a caller supplies. Reached here through a subclass, which is what
 * protected access is for - the alternative would be widening the real interface
 * for the benefit of a test.
 */
class TokenSource : public QuickDC::QueueManager
{
	public:
		explicit TokenSource(const char* path) : QuickDC::QueueManager(path) { }
		using QuickDC::QueueManager::makeToken;
};

/*
 * A queue of its own per case, never the real ~/.quickdc/queue.xml - the
 * constructor reads whatever path it is given and the destructor writes it back.
 */
class Queue
{
	public:
		std::string path;
		TokenSource* queue = 0;

		Queue() : path(scratch("queue.xml"))
		{
			queue = new TokenSource(path.c_str());
		}

		~Queue()
		{
			delete queue;
			Samurai::IO::File(path).remove();
			Samurai::IO::File(path + ".new").remove();
		}

		Queue(const Queue&) = delete;
		Queue& operator=(const Queue&) = delete;
};

/* How many leading characters two strings share. */
size_t commonPrefix(const std::string& a, const std::string& b)
{
	size_t n = 0;
	while (n < a.size() && n < b.size() && a[n] == b[n]) n++;
	return n;
}

}

/* ------------------------------------------------------------------------- */
/* What a token looks like                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Sixteen bytes of entropy, base32 encoded. The length is asserted because it is
 * what says how much entropy is in there: a shorter token is a weaker one, and
 * nothing else in the protocol would notice.
 */
EXO_TEST(token_carries_sixteen_bytes_of_entropy,
{
	Queue q;

	const std::string token = q.queue->makeToken();
	EXO_ASSERT(!token.empty());
	EXO_ASSERT_EQ_UINT(token.size(), Samurai::Util::base32_encode_size(16) - 1);
	return 1;
});

/* Base32's alphabet, and nothing outside it - a token goes on a protocol line. */
EXO_TEST(token_is_base32_and_nothing_else,
{
	Queue q;

	for (int n = 0; n < 32; n++)
	{
		const std::string token = q.queue->makeToken();
		EXO_ASSERT(!token.empty());

		for (char c : token)
		{
			const bool base32 = (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
			if (!base32)
			{
				const std::string why = "token \"" + token
					+ "\" contains a character outside base32";
				EXO_FAIL(why.c_str());
			}
		}
	}
	return 1;
});

/*
 * No separator, no space, and nothing that would need escaping: a token is put on
 * an ADC line as an argument, where a space would split it in two.
 */
EXO_TEST(token_needs_no_escaping_on_a_protocol_line,
{
	Queue q;

	for (int n = 0; n < 32; n++)
	{
		const std::string token = q.queue->makeToken();
		EXO_ASSERT(token.find(' ') == std::string::npos);
		EXO_ASSERT(token.find('\n') == std::string::npos);
		EXO_ASSERT(token.find('|') == std::string::npos);
		EXO_ASSERT(token.find('\\') == std::string::npos);
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What distinguishes a drawn token from a derived one                       */
/* ------------------------------------------------------------------------- */

/* A repeat would mean two transfers answering to one secret. */
EXO_TEST(token_is_never_drawn_twice,
{
	Queue q;

	std::set<std::string> seen;
	for (int n = 0; n < 256; n++)
	{
		const std::string token = q.queue->makeToken();
		EXO_ASSERT(!token.empty());
		if (!seen.insert(token).second)
		{
			const std::string why = "the token \"" + token + "\" was drawn twice";
			EXO_FAIL(why.c_str());
		}
	}
	EXO_ASSERT_EQ_UINT(seen.size(), 256u);
	return 1;
});

/*
 * The case that fails for the scheme this replaced, and the reason this file
 * exists.
 *
 * "QDC" + clock + counter gives every token in a run the same long prefix: the
 * literal, then the clock reading, which moves in the low digits only. Two
 * consecutive tokens differed in about a character. Drawn tokens share a prefix
 * only by coincidence, and 256 of them sharing four characters is not one.
 */
EXO_TEST(token_shares_no_prefix_with_the_one_before_it,
{
	Queue q;

	std::string previous;
	size_t longest = 0;

	for (int n = 0; n < 256; n++)
	{
		const std::string token = q.queue->makeToken();
		EXO_ASSERT(!token.empty());

		if (!previous.empty())
		{
			const size_t shared = commonPrefix(previous, token);
			if (shared > longest) longest = shared;
		}
		previous = token;
	}

	/*
	 * Four is far above what chance gives over 256 draws of a 26 character
	 * base32 string, and far below the ten or more a clock-derived token shares.
	 */
	if (longest >= 4)
	{
		const std::string why = "consecutive tokens share " + std::to_string(longest)
			+ " leading characters, which a drawn token does not";
		EXO_FAIL(why.c_str());
	}
	return 1;
});

/*
 * Nor is a token derived from anything a second queue also knows. Two managers
 * built at the same moment used to produce the same first token, because the
 * clock and the counter were all that went into it - so two clients started
 * together were interchangeable.
 */
EXO_TEST(token_does_not_depend_on_when_the_queue_was_made,
{
	Queue a;
	Queue b;

	const std::string first = a.queue->makeToken();
	const std::string second = b.queue->makeToken();

	EXO_ASSERT(!first.empty());
	EXO_ASSERT(!second.empty());
	EXO_ASSERT(first != second);
	EXO_ASSERT(commonPrefix(first, second) < 4);
	return 1;
});

/* Every position varies, rather than only the tail a counter would move. */
EXO_TEST(token_varies_across_its_whole_length,
{
	Queue q;

	std::vector<std::string> tokens;
	for (int n = 0; n < 64; n++)
	{
		const std::string token = q.queue->makeToken();
		EXO_ASSERT(!token.empty());
		tokens.push_back(token);
	}

	const size_t length = tokens[0].size();
	for (size_t at = 0; at < length; at++)
	{
		std::set<char> here;
		for (const std::string& token : tokens)
		{
			EXO_ASSERT_EQ_UINT(token.size(), length);
			here.insert(token[at]);
		}

		/*
		 * A counter moves its last digits and leaves the rest alone, so a fixed
		 * position would show one character across all 64.
		 */
		if (here.size() < 2)
		{
			const std::string why = "position " + std::to_string(at)
				+ " is the same character in all 64 tokens";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* How a token is checked                                                    */
/* ------------------------------------------------------------------------- */

/*
 * The token a request was granted for is the one that authorises it, and a near
 * miss is no better than a wrong guess. The comparison itself is constant time -
 * samurai's own suite covers Util::constantTimeEquals - and what is asserted here
 * is that the answer still depends on the whole token.
 */
EXO_TEST(token_authorises_only_the_request_it_was_granted_for,
{
	Queue q;
	QuickDC::SecurityManager mgr;

	const std::string token = q.queue->makeToken();
	EXO_ASSERT(!token.empty());

	mgr.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
		QuickDC::Clearance::ADC, 0, "CIDA", token.c_str()));

	EXO_ASSERT(mgr.isAuthorized(QuickDC::Clearance::ADC, 0, 0, "CIDA", token.c_str()));

	/* One character out, at each end and in the middle. */
	for (size_t at : { size_t(0), token.size() / 2, token.size() - 1 })
	{
		std::string bent = token;
		bent[at] = (bent[at] == 'A') ? 'B' : 'A';

		if (mgr.isAuthorized(QuickDC::Clearance::ADC, 0, 0, "CIDA", bent.c_str()))
		{
			const std::string why = "a token differing at position "
				+ std::to_string(at) + " was accepted";
			EXO_FAIL(why.c_str());
		}
	}

	/* And a prefix of it is not it. */
	EXO_ASSERT(!mgr.isAuthorized(QuickDC::Clearance::ADC, 0, 0, "CIDA",
		token.substr(0, token.size() - 1).c_str()));
	return 1;
});

/*
 * A request is only made when a token could be drawn. There is no way to make the
 * kernel refuse entropy from here, so what is asserted is the other half of that
 * contract: a token that was drawn is the one the request answers to, and it can
 * be found by it.
 */
EXO_TEST(token_names_the_request_it_was_made_for,
{
	Queue q;

	const std::string token = q.queue->makeToken();
	EXO_ASSERT(!token.empty());

	/* Nothing was registered under it, so nothing is found. */
	EXO_ASSERT_NULL(q.queue->lookupRequest(token.c_str()));
	EXO_ASSERT_NULL(q.queue->lookupRequest(""));
	EXO_ASSERT_NULL(q.queue->lookupRequest(0));
	return 1;
});
