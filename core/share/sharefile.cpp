/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/core.h"
#include "hash/hashmanager.h"
#include "share/filetypes.h"
#include "share/sharemanager.h"
#include "share/sharedir.h"
#include "share/sharefile.h"
#include <samurai/io/file.h>
#define MAXBUFSIZE 8 * 1024 * 1024

QuickDC::Share::File::File(const std::string& name, QuickDC::Share::Dir* dir) : directory(dir), file(name), publicName(""),
tth(0) {
	popularity = 0;
	ftype = FAny;
	size = file.size();
	directory->addSize(size);

	/* The table holds the flag itself, and it is what decides a search by type. */
	for (size_t i = 0; i < KNOWN_EXTENSIONS; i++)
		if (file.matchExtension(known_ext[i].ext))
			ftype |= known_ext[i].ftype;

	directory->getManager()->hash->addJob(file, this);
}

QuickDC::Share::File::~File() {
	if (!tth) {
		if (QuickDC::Share::Manager* manager = directory ? directory->getManager() : nullptr)
			manager->hash->removeJob(file);
	}
	
	free(tth);
}

/**
 * This will return the public name as perceived by other users.
 *
 * From the directory's public name, which is relative to the share root: this
 * string is the FN field of a search result, and the directory's getName() is
 * the path the share has on this machine.
 */
const std::string& QuickDC::Share::File::getPublicName() {
	if (publicName != "")
		return publicName;

	publicName = "/";
	publicName += directory->getPublicName();
	publicName += "/";
	publicName += file.getBaseName();

	return publicName;
}


int QuickDC::Share::File::getLevel() {
	int level = 0;
	QuickDC::Share::Dir* dir = this->directory;
	while (dir) {
		dir = dir->parent;
		level++;
	}
	return level;
}

void QuickDC::Share::File::EventFileStarted(const QuickDC::Hash::Job*) { }
void QuickDC::Share::File::EventFileQueued(const QuickDC::Hash::Job*) { }
void QuickDC::Share::File::EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) { }

void QuickDC::Share::File::EventFileHashed(const QuickDC::Hash::Job* job)
{
	tth = strdup(job->getHashSum());
	
	if (directory && directory->getManager())
		directory->getManager()->addLookupFile(getBaseFileName().c_str(), tth, this);
}


