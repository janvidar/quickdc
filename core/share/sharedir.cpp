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
	buildPublicName();
	scan();
}

QuickDC::Share::Dir::Dir(const std::string& name_, QuickDC::Share::Dir* parent_) : parent(parent_), manager(0), path(name_), name(name_), size(0), filecount(0) {

	/*
	 * Anything after the last slash is the name; the test is only there to
	 * catch a trailing one, which leaves nothing after it. Requiring more than
	 * one character meant a directory called "a" fell back to the whole path -
	 * and the basename is what goes into the file list handed to strangers.
	 */
	size_t split = name.rfind('/');
	if (split != std::string::npos && !name.substr(split+1).empty())
		basename = name.substr(split+1);
	else
		basename = name;

	buildPublicName();
	scan();
}

/*
 * From the base names alone, so that what a stranger is told stays inside the
 * share: 'name' is the whole path on this machine for anything below the top
 * level, and the top level's is the alias the user chose to publish it under.
 * Done here rather than on demand because scan() builds the children, and each
 * of them composes its own from this one.
 */
void QuickDC::Share::Dir::buildPublicName() {
	publicName = basename;
	for (const QuickDC::Share::Dir* up = parent; up; up = up->parent)
		publicName = up->basename + "/" + publicName;
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
 * A share is published to strangers, so the tree is bounded whatever the disk
 * holds: this is deep enough for any real collection and shallow enough that
 * the recursion below cannot exhaust the stack.
 */
#define MAX_SHARE_DEPTH 64

/*
 * Scan directory for files / directories. Symlinks are not followed - see below.
 */
void QuickDC::Share::Dir::scan() {
	QDBG("Scanning directory: %s", path.c_str());

	if (getLevel() >= MAX_SHARE_DEPTH) {
		QERR("Not descending into '%s': a share may not be more than %d levels deep",
			path.c_str(), MAX_SHARE_DEPTH);
		return;
	}

	Samurai::IO::Directory dir(path);
	if (!dir.open()) {
		QERR("Unable to open directory %s", path.c_str());
		return;
	}

	Samurai::IO::File file;
	for (bool more = dir.first(file); more; more = dir.next(file)) {
		/*
		 * Symlinks first, and skipped. isDirectory() and isRegular() both follow
		 * a link and answer for its target, so a symlink was always classified
		 * as whatever it pointed at and this test could never be reached - which
		 * published files from outside the share root, ~/.ssh/id_rsa among them,
		 * to any peer that asked for the file list. Only isSymlink() has lstat
		 * semantics, so only it can tell.
		 */
		if (file.isSymlink()) {
			QDBG("Ignoring symlink: %s", file.getName().c_str());
			continue;
		}

		if (!file.isReadable()) {
			continue;
		}

		if (file.isDirectory()) {
			QDBG("- Dir %s", file.getName().c_str());
			subdirs.push_back(new QuickDC::Share::Dir(file.getName(), this));
		} else if (file.isRegular()) {
			QDBG("- File %s", file.getName().c_str());
			files.push_back(new QuickDC::Share::File(file.getName(), this));
		}
	}
}


