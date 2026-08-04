/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dc/lock.h"

#include <samurai/util/base32.h>
#include <samurai/util/random.h>

/*
 * How much of the challenge is drawn. Sixteen bytes is past being searched, and
 * base32 of it carries none of the characters NMDC uses as separators - a lock
 * goes out in "$Lock <lock> Pk=<pk>|", so a space, a '$' or a '|' in it would
 * split the command.
 */
#define QUICKDC_LOCK_ENTROPY 16

/*
 * The prefix is protocol, not decoration: a peer reads it to decide whether we
 * speak the extended protocol at all - see clientsession.cpp and hubsession.cpp,
 * which both test starts_with("EXTENDEDPROTOCOL") on what they were sent. Only
 * the tail is ours to choose.
 */
#define QUICKDC_LOCK_PREFIX "EXTENDEDPROTOCOL"

std::string DC::Lock::createLock() {
	unsigned char raw[QUICKDC_LOCK_ENTROPY];
	if (!Samurai::Util::secureRandom(raw, sizeof(raw)))
	{
		/*
		 * Nothing weaker is substituted. This used to return
		 * "EXTENDEDPROTOCOLABCABC" - the same challenge from every instance of
		 * this client, for ever - which is no challenge at all: the key derived
		 * from it is a constant, so it proves nothing about who answered. A
		 * caller that cannot get a fresh one drops the connection rather than
		 * issuing the fixed one again.
		 */
		QERR("Unable to obtain random bytes for a lock");
		return std::string();
	}

	char tail[Samurai::Util::base32_encode_size(QUICKDC_LOCK_ENTROPY)];
	Samurai::Util::base32_encode(raw, tail);
	return std::string(QUICKDC_LOCK_PREFIX) + tail;
}

std::string DC::Lock::createPk() {
	return "QuickDC_" VERSION "_" SYSTEM "/" CPU;
}

std::string DC::Lock::calculateKey(std::string_view lock) {
	if (lock.size() < 3) return std::string();

	/* Each byte is derived from the one before it in the lock, not in the
	   result, so the two cannot be the same buffer. */
	std::string tmp(lock);
	for (size_t i = 1; i < lock.size(); i++)
		tmp[i] = static_cast<char>(lock[i] ^ lock[i-1]);

	// set the first one.
	tmp[0] = static_cast<char>(lock[0] ^ tmp[lock.size()-1] ^ 5);

	for (char& c : tmp)
		c = static_cast<char>(((c << 4) & 0xf0) | ((c >> 4) & 0x0f));

	// escape it.
	std::string key;
	for (char c : tmp) {
		switch (c) {
			case 0:   key += "/%DCN000%/"; break;
			case 5:   key += "/%DCN005%/"; break;
			case 36:  key += "/%DCN036%/"; break;
			case 96:  key += "/%DCN096%/"; break;
			case 124: key += "/%DCN124%/"; break;
			case 126: key += "/%DCN126%/"; break;
			default:
				key += c;
		}
	}

	return key;
}

