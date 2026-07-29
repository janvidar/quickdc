/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "test/cli/util.h"

const char* getTimestamp() {
	static char timestamp[11] = { 0, };
	time_t now = time(0);
	strftime(timestamp, 11, "[%H:%M:%S]", localtime(&now));
	return timestamp;
}
