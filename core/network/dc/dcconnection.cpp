/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dc/dcconnection.h"

/* static */
bool DC::probe(const char* handshake, size_t length) {
	if (length < 8) return false;
	if (strncmp(handshake, "$MyNick ", 8) == 0) return true;
	return false;
}


