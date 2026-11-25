/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_HUBURL_H
#define HAVE_QUICKDC_API_HUBURL_H

#include <string>

namespace QuickDC {

/**
 * The one spelling a hub's address is kept under: trimmed, "dchub://" where no
 * scheme is given, and NMDC's port 411 where none is. What it cannot make sense
 * of is left as it was rather than dropped. Free of everything else in the core,
 * so a front end may ask it too.
 */
std::string canonicalHubUrl(const std::string& url);

}

#endif // HAVE_QUICKDC_API_HUBURL_H
