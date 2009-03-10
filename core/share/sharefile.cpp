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

	for (size_t i = 0; i < KNOWN_EXTENSIONS; i++)
		if (file.matchExtension(known_ext[i].ext))
			ftype |= typeLookup[known_ext[i].ftype];

	QuickDC::Share::Dir* d = directory;
	while (!d->manager) d = d->parent;
	d->manager->hash->addJob(file, this);
}

QuickDC::Share::File::~File() {
	if (!tth) {
		QuickDC::Share::Dir* d = directory;
		while (d && !d->manager) d = d->parent;
		if (d)
			d->manager->hash->removeJob(file);
	}
	
	free(tth);
}

/**
 * This will return the public name as perceived by other users.
 */
const std::string& QuickDC::Share::File::getPublicName() {
	if (publicName != "")
		return publicName;
	
	publicName += directory->getName();
	publicName += "/";
	publicName += file.getBaseName();
	
	if (publicName[0] != '/')
		publicName = '/' + publicName;
	
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
	
	if (QuickDC::Core::getInstance()->shares)
	{
		QuickDC::Core::getInstance()->shares->addLookupFile(getBaseFileName().c_str(), tth, this);
	}
}


