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


QuickDC::Hash::Manager::Manager(const char* cache_file)
	: cachefile(cache_file ? cache_file : FILE_HASH_CACHE)
	, dirty(false)
	, idle_announced(true)
	, inflight(0)
	, stopping(false)
	, progress_done(0)
	, progress_total(0)
{
	loadCache();
	worker = std::thread(&QuickDC::Hash::Manager::workerLoop, this);
}


QuickDC::Hash::Manager::~Manager()
{
	{
		std::lock_guard<std::mutex> guard(lock);
		stopping = true;
	}
	wakeup.notify_all();
	if (worker.joinable())
		worker.join();

	/* The worker is gone, so these need no locking any more. Jobs still in
	   flight are dropped without notifying their listeners - by this point the
	   share manager they point into is already being torn down. */
	while (!pending.empty())
	{
		delete pending.front();
		pending.pop_front();
	}

	while (!completed.empty())
	{
		delete completed.front().job;
		completed.pop_front();
	}

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
	std::lock_guard<std::mutex> guard(lock);
	return inflight || !pending.empty() || !completed.empty();
}


void QuickDC::Hash::Manager::workerLoop()
{
	for (;;)
	{
		QuickDC::Hash::Job* job = 0;

		{
			std::unique_lock<std::mutex> guard(lock);
			while (!stopping && pending.empty())
				wakeup.wait(guard);

			if (stopping)
				return;

			job = pending.front();
			pending.pop_front();
			inflight = job;
			progress_done.store(0);
			progress_total.store(0);
		}

		bool ok = job->runToCompletion(progress_done, progress_total);

		{
			std::lock_guard<std::mutex> guard(lock);
			inflight = 0;

			Completion done;
			done.job = job;
			done.ok = ok;
			completed.push_back(done);
		}
	}
}


bool QuickDC::Hash::Manager::process()
{
	std::deque<Completion> done;
	QuickDC::Hash::Job* running = 0;
	bool more = false;

	{
		std::lock_guard<std::mutex> guard(lock);
		done.swap(completed);
		running = inflight;
		more = (running != 0) || !pending.empty();
	}

	/* 'running' stays valid without the lock: only this thread deletes jobs,
	   and a job the worker finishes meanwhile just moves to 'completed' to be
	   picked up by the next call. */
	if (running)
		running->getListener()->EventFileHashProgress(running, progress_done.load(), progress_total.load());

	while (!done.empty())
	{
		Completion c = done.front();
		done.pop_front();

		if (c.ok)
		{
			QDBG("Hash job finished, file='%s'", c.job->getFile().getName().c_str());
			QuickDC::Hash::CacheEntry* entry = QuickDC::Hash::CacheEntry::create(c.job->getFile(), c.job);
			addCache(c.job->getFile().getName(), entry);
			c.job->getListener()->EventFileHashed(c.job);
		}
		else
		{
			QERR("Hash job aborted for '%s': %d: %s",
				c.job->getFile().getName().c_str(), c.job->getError(), strerror(c.job->getError()));
		}

		delete c.job;
	}

	if (!more)
	{
		/* Announce completion once, on the edge where the last job was
		   collected. process() is called every main loop iteration, so without
		   the latch an idle client would re-save the cache forever. */
		if (!idle_announced)
		{
			idle_announced = true;
			QDBG("*** All hash jobs are now complete!");
			saveCache();

			// Make sure we regenerate the file lists since we now have all TTH roots.
			/* A manager pointed at a cache of its own runs without a core at
			   all, and the core has no share manager until it has built one. */
			QuickDC::Core* core = QuickDC::Core::getInstance();
			if (core && core->shares)
				core->shares->createFileLists(false);
		}
		return false;
	}

	return true;
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

	{
		std::lock_guard<std::mutex> guard(lock);
		pending.push_back(job);
		idle_announced = false;
	}
	wakeup.notify_one();

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
Samurai::IO::File* QuickDC::Hash::Manager::getLeafData(const Samurai::IO::File& file, uint64_t& offset, uint64_t& length)
{
	QuickDC::Hash::CacheEntry* entry = 0;
	if (!lookup(file, entry) || !entry) return 0;
	if (!entry->getLeafdataSize()) return 0;

	offset = (uint64_t) entry->getCacheFileOffset();
	length = (uint64_t) entry->getLeafdataSize();
	return getCacheFile();
}

Samurai::IO::File* QuickDC::Hash::Manager::getCacheFile()
{
	Samurai::IO::File* cfile = new Samurai::IO::File(cachefile);
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
	
	QuickDC::Hash::CacheStorage store{Samurai::IO::File(cachefile)};
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
		Samurai::IO::File cache_new(cachefile + ".new");
		Samurai::IO::File cache_old(cachefile);
		
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

/* The manager used to post itself a message to keep process() being called.
   That is redundant now: the main loop calls process() every iteration, and
   the hashing it used to drive happens on the worker thread. */