/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dc/lock.h"
#include <samurai/io/buffer.h>


char* DC::Lock::createLock() {
	char* lock = strdup("EXTENDEDPROTOCOLABCABC");
	return lock;
}

char* DC::Lock::createPk() {
	char* pk = strdup("QuickDC_" VERSION "_" SYSTEM "/" CPU);
	return pk;
}

char* DC::Lock::calculateKey(const char* lock, size_t length) {
	if (length < 3) return 0;
	char* tmp = (char*) malloc(length);
	size_t i;

	for (i = 1; i < length; i++)
		tmp[i] = lock[i] ^ lock[i-1];
	
	// set the first one.
	tmp[0] = lock[0] ^ tmp[length-1] ^ 5;
	
	for (i = 0; i < length; i++)
		tmp[i] = (((tmp[i]<<4) & 0xf0) | ((tmp[i]>>4) & 0x0f));

	// escape it.
	Samurai::IO::Buffer* output = new Samurai::IO::Buffer();
	for (i = 0; i < length; i++) {
		switch (tmp[i]) {
			case 0:   output->append("/%DCN000%/"); break;
			case 5:   output->append("/%DCN005%/"); break;
			case 36:  output->append("/%DCN036%/"); break;
			case 96:  output->append("/%DCN096%/"); break;
			case 124: output->append("/%DCN124%/"); break;
			case 126: output->append("/%DCN126%/"); break;
			default:
				output->append(tmp[i]);
		}
	}
	output->append('\0');
	
	char* key = (char*) malloc(output->size());
	output->pop(key, output->size());
	delete output;

	free(tmp);
	return key;
}

