/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dc/dcconnection.h"
#include <string_view>

/* static */
bool DC::probe(const char* handshake, size_t length) {
	if (!handshake || length < 8) return false;
	return std::string_view(handshake, length).starts_with("$MyNick ");
}


