/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HASH_MANAGER_H
#define HAVE_QUICKDC_HASH_MANAGER_H

#include "quickdc.h"
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

#include <samurai/crypto/digest/hash.h>
#include <samurai/crypto/digest/tigertree.h>

#define DEFAULT_HASH_BUFFER_SIZE 1024*1024

namespace Samurai {
	namespace IO {
		class File;
		class Buffer;
	}
}

namespace QuickDC {
namespace Hash {

class JobListener;
class Job;
class CacheEntry;

enum HashType { HashTiger, HashTigerTree, HashMD5, HashSHA1, HashSHA256 };
size_t getHashSize(enum HashType type);


/**
 * This class handles asynchronous hashing of files using the TTH
 * hash.
 * It hash it's own work queue and cache management. 
 * The idea is that a file does not need to be hashed more than once
 * when we beleive there has been no change to it.
 * Therefore, when a file has been hashed, it will be added to the hash
 * cache.
 *
 * The hash cache is a very simple text file on the format:
 *
 * size:modifydate:TTH:sum:filename
 *
 * If size and/or modifydate are changed the cache is invalidated, and
 * the file will be added to the work queue for hashing.
 *
 */
/**
 * Hashing itself runs on a single worker thread: Job::runToCompletion() is
 * pure CPU and file I/O, so keeping it off the main thread stops it from
 * competing with the networking loop (and stops the loop from having to spin
 * to keep hashing moving).
 *
 * The split is deliberately narrow. The worker only ever touches the job it
 * has taken off 'pending'. Everything with side effects outside that job -
 * the cache, the listener callbacks, the share file lists, the debug log -
 * stays on the main thread, which picks finished jobs up from 'completed' in
 * process().
 */
class Manager
{
	public:
		Manager();
		virtual ~Manager();

		/**
		 * Add a cache job to the manager.
		 */
		void addJob(const Samurai::IO::File&, QuickDC::Hash::JobListener*);
		void removeJob(const Samurai::IO::File&);

		/**
		 * Returns true if more work remains, or false
		 * if no more work needs to be done at the moment.
		 */
		bool isBusy() const;

		/**
		 * Announce whatever the worker thread has finished, and report
		 * progress for the job it is on. Cheap to call: it does no hashing,
		 * so the main loop can call it every iteration.
		 *
		 * Returns true if more work remains, or false
		 * if no more work needs to be done at the moment.
		 */
		bool process();
		
		/**
		 * Add a file to the cache.
		 */
		void addCache(const std::string& filename, QuickDC::Hash::CacheEntry* entry);
		
		/**
		 * Lookup a specific file.
		 * If the file is found in the cache and a file_offset exists, this method
		 * return true and the file offset within the cache file where the data section is.
		 * Otherwise, false is returned.
		 */
		bool lookup(const Samurai::IO::File& file, QuickDC::Hash::CacheEntry*& cache);
		
		Samurai::IO::File* getCacheFile();

		/**
		 * Where a shared file's leaf hashes sit inside the cache file, for
		 * serving them to a peer.
		 *
		 * @return the cache file, which the manager owns, or 0 if we have no
		 *         leaves for it.
		 */
		Samurai::IO::File* getLeafData(const Samurai::IO::File& file, uint64_t& offset, uint64_t& length);
		
	protected:
		void loadCache();
		void saveCache();

		/** The worker thread body. */
		void workerLoop();

		struct Completion {
			QuickDC::Hash::Job* job;
			bool ok;
		};

	protected:
		/* Main thread only. */
		std::map<std::string, QuickDC::Hash::CacheEntry*> cache;
		bool dirty;

		/* Latches the "all jobs complete" work so it fires on the edge rather
		   than on every idle call. Starts true: an idle client that never
		   queues anything has nothing to announce. */
		bool idle_announced;

		/* Shared with the worker thread, guarded by 'lock'. */
		mutable std::mutex lock;
		std::condition_variable wakeup;
		std::deque<QuickDC::Hash::Job*> pending;
		std::deque<Completion> completed;
		QuickDC::Hash::Job* inflight;
		bool stopping;

		/* Written by the worker, read by the main thread for progress. */
		std::atomic<uint64_t> progress_done;
		std::atomic<uint64_t> progress_total;

		std::thread worker;
};

}
}

#endif // HAVE_QUICKDC_HASH_MANAGER_H
