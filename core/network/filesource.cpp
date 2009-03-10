/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/filesource.h"
#include <string.h>
#include <stdlib.h>
#include <vector>

QuickDC::FileSource::FileSource(const char* url_) : hub(0), cid(0), filename(0) {
	hub = strdup(url_);
}
		
QuickDC::FileSource::FileSource(const char* hub_, const char* cid_, const char* filename_) {
	hub = strdup(hub_);
	cid = strdup(cid_);
	filename = (filename_) ? strdup(filename_) : 0;
}
		
QuickDC::FileSource::~FileSource() {
	free(hub);
	free(cid);
	free(filename);
}




