/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "share/sharefile.h"
#include "share/sharedir.h"
#include "share/sharemanager.h"
#include "config/preferences.h"
#include "api/core.h"
#include "hash/hashmanager.h"
#include "share/dcfilelist.h"

#include <samurai/io/buffer.h>
#include <samurai/io/file.h>


void calculateLocalShares()
{
	QuickDC::Preferences config("~/.quickdc/quickdc.conf");
	QuickDC::Hash::Manager hash;
	QuickDC::Share::Manager shares(&config, &hash);
	
	shares.initialize();
	QDBG("calculateLocalShares()");
	
	while (hash.isBusy())
	{
		hash.process();
	}
	
	shares.createFileLists(false);
	
	printf("Shared: %s bytes, %d files\n", quickdc_ulltoa(shares.getSharedSize()), shares.getFiles());
}


void parseShareFile(const char* file_) {
	printf("Opening share file: %s\n", file_);
	
	Samurai::IO::File file(file_);
	Share::RemoteFileList* list = new Share::RemoteFileList(&file);
	
	bool ok = list->decode();
	if (!ok)
	{
		QERR("Decode failed");
	}
	else
	{
		size_t n = list->root->getSize();
		printf("File list successfully decoded...\n");
		printf("Shared: %s bytes, %d files\n", quickdc_ulltoa(n), (int) list->root->getFiles());
	}
	delete list;
}


int main(int argc, char** argv)
{
	QDBG_INIT;
	if (argc < 2)
	{
		calculateLocalShares();
	}
	else
	{
		parseShareFile(argv[1]);
	}
	QDBG_FINI;
}

// eof
