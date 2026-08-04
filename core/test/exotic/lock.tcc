/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The NMDC lock and key.
 *
 * NMDC's handshake has each side send a "lock" and expect the other to answer
 * with a "key" derived from it. It authenticates nothing: the derivation is an
 * unkeyed, fully invertible XOR chain plus a nibble swap, so anyone who sees the
 * lock can compute the key. That is inherent to the protocol and is not what
 * these cases are about.
 *
 * What they are about is the one part that was ours to get right. createLock()
 * returned the same twenty-two characters from every instance of this client, for
 * ever - so the "challenge" was a constant, and the key answering it was a
 * constant too. It is now drawn per connection. A fresh lock does not make NMDC
 * secure; it stops this client from being the one thing in the exchange that a
 * peer could have predicted without trying.
 *
 * The escaping rules the key goes through are the other half, and they matter for
 * a different reason: a key that is not escaped can carry the bytes NMDC uses as
 * separators, and a '|' in a key ends the command early.
 */

#include "quickdc.h"
#include "network/dc/lock.h"

#include <set>
#include <string>
#include <vector>

namespace {

/* The prefix a peer reads to decide whether we speak the extended protocol. */
const char* EXTENDED = "EXTENDEDPROTOCOL";

}

/* ------------------------------------------------------------------------- */
/* The lock we issue                                                         */
/* ------------------------------------------------------------------------- */

/*
 * The prefix is protocol rather than decoration: clientsession.cpp and
 * hubsession.cpp both test starts_with("EXTENDEDPROTOCOL") on what they were
 * sent, so dropping it would silently turn off every extension.
 */
EXO_TEST(lock_keeps_the_extended_protocol_prefix,
{
	const std::string lock = DC::Lock::createLock();
	EXO_ASSERT(!lock.empty());
	EXO_ASSERT(lock.compare(0, strlen(EXTENDED), EXTENDED) == 0);
	return 1;
});

/*
 * A lock goes out inside "$Lock <lock> Pk=<pk>|", so any of those three
 * characters in it would split the command - and the tail is drawn, so this has
 * to hold for every draw rather than for one.
 */
EXO_TEST(lock_carries_nothing_that_would_split_the_command,
{
	for (int n = 0; n < 64; n++)
	{
		const std::string lock = DC::Lock::createLock();
		EXO_ASSERT(!lock.empty());

		if (lock.find(' ') != std::string::npos
			|| lock.find('|') != std::string::npos
			|| lock.find('$') != std::string::npos)
		{
			const std::string why = "the lock \"" + lock
				+ "\" contains a character NMDC uses as a separator";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/* The case that fails for the constant this replaced. */
EXO_TEST(lock_is_not_the_same_challenge_twice,
{
	std::set<std::string> seen;
	for (int n = 0; n < 128; n++)
	{
		const std::string lock = DC::Lock::createLock();
		EXO_ASSERT(!lock.empty());

		if (!seen.insert(lock).second)
		{
			const std::string why = "the lock \"" + lock + "\" was issued twice";
			EXO_FAIL(why.c_str());
		}
	}
	EXO_ASSERT_EQ_UINT(seen.size(), 128u);
	return 1;
});

/*
 * And the part after the prefix is what varies - a lock that differed only in a
 * trailing counter would still be predictable from one observation.
 */
EXO_TEST(lock_varies_across_the_whole_drawn_tail,
{
	std::vector<std::string> tails;
	for (int n = 0; n < 64; n++)
	{
		const std::string lock = DC::Lock::createLock();
		EXO_ASSERT(!lock.empty());
		EXO_ASSERT(lock.size() > strlen(EXTENDED));
		tails.push_back(lock.substr(strlen(EXTENDED)));
	}

	const size_t length = tails[0].size();
	EXO_ASSERT(length >= 8);

	for (size_t at = 0; at < length; at++)
	{
		std::set<char> here;
		for (const std::string& tail : tails)
		{
			EXO_ASSERT_EQ_UINT(tail.size(), length);
			here.insert(tail[at]);
		}

		if (here.size() < 2)
		{
			const std::string why = "position " + std::to_string(at)
				+ " of the drawn tail is the same character in all 64 locks";
			EXO_FAIL(why.c_str());
		}
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The key derived from one                                                  */
/* ------------------------------------------------------------------------- */

/* A different lock gives a different key, or the challenge means nothing. */
EXO_TEST(lock_a_fresh_lock_derives_a_fresh_key,
{
	std::set<std::string> keys;
	for (int n = 0; n < 64; n++)
	{
		const std::string lock = DC::Lock::createLock();
		EXO_ASSERT(!lock.empty());

		const std::string key = DC::Lock::calculateKey(lock);
		EXO_ASSERT(!key.empty());
		keys.insert(key);
	}
	EXO_ASSERT_EQ_UINT(keys.size(), 64u);
	return 1;
});

/* The derivation is a function of the lock alone, so it repeats for one. */
EXO_TEST(lock_the_same_lock_derives_the_same_key,
{
	const std::string lock = DC::Lock::createLock();
	EXO_ASSERT(!lock.empty());

	const std::string first = DC::Lock::calculateKey(lock);
	const std::string second = DC::Lock::calculateKey(lock);
	EXO_ASSERT(!first.empty());
	EXO_ASSERT(first == second);
	return 1;
});

/*
 * A key goes out in "$Key <key>|" unescaped, so the escaping is what keeps the
 * bytes NMDC reserves out of it. The five it replaces are the ones that would
 * otherwise end the command or start a field.
 */
EXO_TEST(lock_a_key_carries_no_reserved_byte,
{
	for (int n = 0; n < 64; n++)
	{
		const std::string lock = DC::Lock::createLock();
		const std::string key = DC::Lock::calculateKey(lock);
		EXO_ASSERT(!key.empty());

		for (unsigned char c : key)
		{
			if (c == 0 || c == 5 || c == 36 || c == 96 || c == 124 || c == 126)
			{
				const std::string why = "a key contains the reserved byte "
					+ std::to_string((int) c);
				EXO_FAIL(why.c_str());
			}
		}
	}
	return 1;
});

/* Too short to derive from is refused rather than answered with something. */
EXO_TEST(lock_a_lock_too_short_to_derive_from_is_refused,
{
	EXO_ASSERT(DC::Lock::calculateKey("").empty());
	EXO_ASSERT(DC::Lock::calculateKey("a").empty());
	EXO_ASSERT(DC::Lock::calculateKey("ab").empty());

	/* Three is the shortest it will work from. */
	EXO_ASSERT(!DC::Lock::calculateKey("abc").empty());
	return 1;
});

/*
 * The Pk field, which carries our build. Asserted only to be non-empty and free
 * of separators: what it says about the host is a disclosure question recorded
 * separately, not something to pin here.
 */
EXO_TEST(lock_the_pk_field_is_usable_on_a_command_line,
{
	const std::string pk = DC::Lock::createPk();
	EXO_ASSERT(!pk.empty());
	EXO_ASSERT(pk.find(' ') == std::string::npos);
	EXO_ASSERT(pk.find('|') == std::string::npos);
	EXO_ASSERT(pk.find('$') == std::string::npos);
	return 1;
});
