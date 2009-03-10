/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_COMMANDPARSER_H
#define HAVE_QUICKDC_COMMANDPARSER_H

#include "network/commandproxy.h"

namespace QuickDC {

class CommandParser {
	public:
		CommandParser();
		virtual ~CommandParser();
};

}

#endif // HAVE_QUICKDC_COMMANDPARSER_H
