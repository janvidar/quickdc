/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include "backend.h"
#include <stdio.h>
#include <samurai/io/file.h>
#include <samurai/io/buffer.h>

#define BUFSIZE 8192

namespace QuickDC {

Backend::Backend(const std::string& file_) {
	file = new Samurai::IO::File(file_.c_str());
	buffer = new Samurai::IO::Buffer();

#ifdef DEBUG
	if (!file->exists()) {
		QDBG("Configuration file does not exist: %s", file_.c_str());
	}
#endif
}

Backend::~Backend() {
	delete buffer;
	delete file;
}

/**
 * FIXME: This is a blocking operation, but
 * it is maybe safe to assume that it doesn't block for too long.
 */
bool Backend::write() {
	if (buffer->size() == 0) return true;
	
	// If file does not exist, let's create it.
	// If it does exist, let's truncate it (overwrite).
	bool ok = file->open(Samurai::IO::File::Write | Samurai::IO::File::Truncate);
	if (!ok) return false;

	size_t length = BUFSIZE;
	char* data = new char[length];

	
	while (buffer->size() > 0) {
		if (buffer->size() < BUFSIZE) length = buffer->size();
		buffer->pop(data, length);
		int written = file->write(data, length);
		
		if (written == -1) {
#ifdef DEBUG
			QDBG("Backend::write(): Write error: %i: '%s'", errno, strerror(errno));
#endif
			return false;
		} 
		buffer->remove(written);
	}
	
	delete[] data;
	
	return file->close();
}

bool Backend::read() {
	if (!file->exists()) return false;
	if (!file->open(Samurai::IO::File::Read)) return false;
	buffer->clear();
	
	size_t length = BUFSIZE;
	char* data = new char[length];
	
	int ret;
	while ((ret = file->read(data, length)) > 0) {
		buffer->append(data, ret);
	}
	
	delete[] data;
	if (ret == -1) return false;
	
	return file->close();
}

}
