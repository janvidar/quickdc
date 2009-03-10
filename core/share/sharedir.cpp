/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <sys/types.h>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <samurai/io/dir.h>
#include "share/sharedir.h"
#include "share/sharefile.h"
#include "share/sharemanager.h"

#define DATADUMP

QuickDC::Share::Dir::Dir(const std::string& path_, const std::string& name_, QuickDC::Share::Manager* manager_) : parent(0), manager(manager_), path(path_), name(name_), size(0), filecount(0) {
	basename = name;
	scan();
}

QuickDC::Share::Dir::Dir(const std::string& name_, QuickDC::Share::Dir* parent_) : parent(parent_), manager(0), path(name_), name(name_), size(0), filecount(0) {

	size_t split = name.rfind('/');
	if (split != std::string::npos && name.substr(split+1).size() > 1)
		basename = name.substr(split+1);
	else
		basename = name;
	
	scan();
}

QuickDC::Share::Dir::~Dir() {
	if (manager)
		manager->removeShare(this);
		
	while (files.size()) {
		QuickDC::Share::File* file = files.back();
		files.pop_back();
		delete file;
	}
	
	while (subdirs.size()) {
		QuickDC::Share::Dir* dir = subdirs.back();
		subdirs.pop_back();
		delete dir;
	}
}

void QuickDC::Share::Dir::addSize(uint64_t sz) {
	size += sz;
	if (parent) parent->addSize(sz);
	else if (manager) manager->addSize(sz);	
	
	filecount++;
}

int QuickDC::Share::Dir::getLevel() {
	int level = 0;
	QuickDC::Share::Dir* dir = this;
	while (dir) {
		dir = dir->parent;
		level++;
	}
	return level-1;
}

QuickDC::Share::Dir* QuickDC::Share::Dir::getDir(const char* name) {
	for (size_t i = 0; i < subdirs.size(); i++) {
#ifdef DATADUMP
		QDBG("getDir: checking: '%s'", subdirs[i]->getBaseName().c_str());
#endif
		if (subdirs[i]->getBaseName() == name)
			return subdirs[i];
	}
#ifdef DATADUMP
	QDBG("getDir: failed!");
#endif
	return 0;
}

QuickDC::Share::File* QuickDC::Share::Dir::getFile(const char* name) {
	for (size_t i = 0; i < files.size(); i++) {
#ifdef DATADUMP
		QDBG("getFile: checking: '%s'", files[i]->getBaseFileName().c_str());
#endif
		if (files[i]->getBaseFileName() == name)
			return files[i];
	}
#ifdef DATADUMP
	QDBG("getFile: failed!");
#endif
	return 0;
}

/*
 *Scan directory for files / directories.
 * FIXME: Add support for symlinks.
 */
void QuickDC::Share::Dir::scan() {
	QDBG("Scanning directory: %s", path.c_str());

	Samurai::IO::Directory dir(path);
	if (!dir.open()) {
		QERR("Unable to open directory %s", path.c_str());
		return;
	}

	Samurai::IO::File* file = dir.first();
	while (file) {
		if (!file->isReadable()) {
			file = dir.next();
			continue;
		}

		if (file->isDirectory()) {
			QDBG("- Dir %s", file->getName().c_str());
			subdirs.push_back(new QuickDC::Share::Dir(file->getName(), this));
		} else if (file->isRegular()) {
			QDBG("- File %s", file->getName().c_str())
			files.push_back(new QuickDC::Share::File(file->getName(), this));

		} else if (file->isSymlink()) {
			QDBG("Ignoring symlink: %s", file->getName().c_str());
		}
		file = dir.next();
	}
}


