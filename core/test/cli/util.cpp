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

const char* getFormatSize(uint64_t size) {
	const char* sizes[7] = { " B", "KB", "MB", "GB", "TB", "PB", "EB" };
	static char buf[64] = { 0, };
	size_t sindex = 0;
	uint64_t base;
	for (base = 1024; size > base; base *= 1024) { sindex++; }
	if (size % base == 0)
		sprintf(buf, "%4d %s", (int) ((double) size / (double) (base / 1024)), sizes[sindex]);
	else
		sprintf(buf, "%.02f %s", (double) size / (double) (base / 1024), sizes[sindex]);
	return buf;
}

