/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DC_CONNECTION_H
#define HAVE_QUICKDC_DC_CONNECTION_H

#include "quickdc.h"

namespace DC {
	bool probe(const char* handshake, size_t length);
}

#endif // HAVE_QUICKDC_DC_CONNECTION_H
