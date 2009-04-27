/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <stdio.h>
#include <errno.h>

#include "hash/hashmanager.h"
#include "hash/hashcache.h"
#include "hash/hashjob.h"
#include "hash/hashstorage.h"
#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include "api/core.h"
#include "share/sharemanager.h"
#include <samurai/util/base32.h>

#define FILE_HASH_CACHE     "~/.quickdc/hashdb.cache"
#define FILE_HASH_CACHE_OLD "~/.quickdc/hashdb.cache.old"
#define FILE_HASH_CACHE_NEW "~/.quickdc/hashdb.cache.new"


enum HashType { HashTiger, HashTigerTree, HashMD5, HashSHA1, HashSHA256 };
size_t QuickDC::Hash::getHashSize(enum HashType type)
{
	switch (type)
	{
		case HashTiger:
		case HashTigerTree:
			return (192/8);
			
		case HashMD5:
			return (128/8);
			
		case HashSHA1:
			return (160/8);
			
		case HashSHA256:
			return (256/8);
	}
	
	/* unknown hash algorithm */
	return 0;
}


QuickDC::Hash::Manager::Manager()
	: current(0)
	, dirty(false)
{
	loadCache();
}


QuickDC::Hash::Manager::~Manager()
{
	saveCache();
	
	while (!cache.empty())
	{
		std::map<std::string, QuickDC::Hash::CacheEntry*>::iterator it = cache.begin();
		QuickDC::Hash::CacheEntry* entry = (*it).second;
		cache.erase(it);
		delete entry;
	}
}


bool QuickDC::Hash::Manager::isBusy() const
{
	return current || jobs.size();
}


bool QuickDC::Hash::Manager::process()
{
	if (jobs.size())
	{
		if (!current)
			current = jobs[0];
		
		int ret = current->process();
		if (ret < 0) {
			QERR("Hash job aborted, an error occured");
			jobs.erase(jobs.begin());
			delete current; current = 0;
			
		}
		else if (ret == 0)
		{
			QDBG("Hash job finished");
			jobs.erase(jobs.begin());
			delete current; current = 0;
		}
		
		// start new hash job
		if (!current && !jobs.size())
		{
			QDBG("*** All hash jobs are now complete!");
			saveCache();
			
			// Make sure we regenerate the file lists since we now have all TTH roots.
			QuickDC::Core::getInstance()->shares->createFileLists(false);
			
			return false;
		}
		return true;
	}
	return false;
}


void QuickDC::Hash::Manager::addJob(const Samurai::IO::File& jobfile, QuickDC::Hash::JobListener* listener)
{
	if (cache.count(jobfile.getName()))
	{
		QuickDC::Hash::CacheEntry* cached = cache[jobfile.getName()];
		QuickDC::Hash::SimpleJob* job = new QuickDC::Hash::SimpleJob(this, listener, cached);
		listener->EventFileHashed(job);
		delete job;
		return;
	}
	
	QuickDC::Hash::Job* job = new QuickDC::Hash::Job(this, QuickDC::Hash::HashTigerTree, listener, jobfile);
	jobs.push_back(job);
	scheduleWork();
	listener->EventFileQueued(job);
}


void QuickDC::Hash::Manager::removeJob(const Samurai::IO::File& f)
{
	(void) f;
	QERR("QuickDC::Hash::Manager::removeJob -- FIXME FIXME");
	assert(!"not implemented");
	dirty = true;
}


void QuickDC::Hash::Manager::addCache(const std::string& file, QuickDC::Hash::CacheEntry* entry)
{
	QDBG("addCache: %s (%p)", file.c_str(), entry);
	if (cache.count(file) > 0)
	{
		QDBG("Already in cache, removing old entry. File='%s'", file.c_str());
		QuickDC::Hash::CacheEntry* old = cache[file];
		cache.erase(file);
		delete old;
	}
	cache[file] = entry;
	dirty = true;
}

bool QuickDC::Hash::Manager::lookup(const Samurai::IO::File& file, QuickDC::Hash::CacheEntry*& ptr)
{
	ptr = 0;
	if (cache.count(file.getName()))
	{
		QuickDC::Hash::CacheEntry* entry = cache[file.getName()];
		QDBG("QuickDC::Hash::Manager::lookupFile found='%s'", file.getName().c_str());
		ptr = entry;
	}
	return ptr != 0;
}

// FIXME: Make sure we don't return this pointer if the file is busy,
//        or *maybe* when it is used by someone else.
Samurai::IO::File* QuickDC::Hash::Manager::getCacheFile()
{
	Samurai::IO::File* cfile = new Samurai::IO::File(FILE_HASH_CACHE);
	return cfile;
}



#define BUFSIZE 8192

/**
 * TODO:
 * Verify the following:
 * - A file with the same name exists.
 *   - If not, delete the cache entry.
 * - size and mtime is unchanged
 *   - If not delete the cache entry, and schedule a new hashjob.
 *
 * - If all worked well so far, add the entry to the cache.
*/
void QuickDC::Hash::Manager::loadCache()
{
	QDBG("QuickDC::Hash::Manager::loadCache()");
	
	QuickDC::Hash::CacheStorage store(Samurai::IO::File(FILE_HASH_CACHE));
	if (!store.open(CacheStorage::ReadAccess))
	{
		QERR("-- Unable to open existing hash database cache");
		return;
	}
	
	QuickDC::Hash::CacheEntry* entry = 0;
	while (store.read(entry))
	{
		if (entry)
		{
			addCache(entry->getFile().getName(), entry);
		}
	}
	dirty = false;
}


/**
 * If bad performance increase write buffer size.
 */
void QuickDC::Hash::Manager::saveCache()
{
	if (dirty)
	{
		QDBG("QuickDC::Hash::Manager::saveCache()");
		Samurai::IO::File cache_new(FILE_HASH_CACHE_NEW);
		Samurai::IO::File cache_old(FILE_HASH_CACHE);
		
		QuickDC::Hash::CacheStorage store(cache_new);
		if (!store.open(CacheStorage::WriteAccess))
		{
			QERR("-- Cannot create a new hash database cache");
			return;
		}
		
		for (std::map<std::string, QuickDC::Hash::CacheEntry*>::iterator it = cache.begin(); it != cache.end(); it++)
		{
			QuickDC::Hash::CacheEntry* entry = (*it).second;
			if (!store.write(entry))
			{
				QERR("HashManager::saveCache(): Write error: %i: '%s'", errno, strerror(errno));
				break;
			}
		}
		store.close();
		
		if (cache_old.exists())
		{
			QDBG("Cache exists, removing old");
			cache_old.remove();
		}
		
		QDBG("Renaming file");
		cache_new.rename(cache_old.getName());
		
		dirty = false;
	}
}

#define MSG_HASH_MANAGER_PROCESS 991299

void QuickDC::Hash::Manager::scheduleWork()
{
	postMessage(MSG_HASH_MANAGER_PROCESS, this, 0, 0);
}

bool QuickDC::Hash::Manager::EventMessage(const Samurai::Message* msg)
{
	if (msg->getID() == MSG_HASH_MANAGER_PROCESS)
	{
		if (process())
		{
			scheduleWork();
		}
		return true;
	}
	return false;
}