/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dc/lock.h"


std::string DC::Lock::createLock() {
	return "EXTENDEDPROTOCOLABCABC";
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

