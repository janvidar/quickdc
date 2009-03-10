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
class Manager {
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
		 * Process the hash jobs.
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
		
	protected:
		void loadCache();
		void saveCache();

	protected:
		std::vector<QuickDC::Hash::Job*> jobs;
		std::map<std::string, QuickDC::Hash::CacheEntry*> cache;
		QuickDC::Hash::Job* current;
		bool dirty;
};

}
}

#endif // HAVE_QUICKDC_HASH_MANAGER_H
