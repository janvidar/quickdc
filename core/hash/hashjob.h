/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HASH_JOB_H
#define HAVE_QUICKDC_HASH_JOB_H

#include "quickdc.h"
#include "hash/hashmanager.h"
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <optional>

#include <samurai/io/file.h>
#include <samurai/crypto/digest/hash.h>
#include <samurai/crypto/digest/tigertree.h>

namespace Samurai {
namespace IO {
	class File;
	class Buffer;
}
}

/**
 * Levels of the tiger tree to keep, so 2^(n-1) leaves at most. This is how
 * finely a download can be verified and re-shared: at 7 a 1GB file has 16MB
 * leaves, at 13 it has 256KB ones and the tree stays under 100KB.
 */
#define QUICKDC_TTH_TREE_LEVELS 13

namespace QuickDC {
namespace Hash {

class Manager;
class Job;

typedef uint8_t tthsum_t;

/**
 * Implement this to receive notifications about hash
 * job progress.
 */
class JobListener
{
	public:
		virtual ~JobListener() { }

		/**
		 * A hash job has been started.
		 */
		virtual void EventFileStarted(const QuickDC::Hash::Job*) = 0;
		
		/**
		 * A hash job has been queued.
		 */
		virtual void EventFileQueued(const QuickDC::Hash::Job*) = 0;
		
		/**
		 * File hashed completely.
		 */
		virtual void EventFileHashed(const QuickDC::Hash::Job*) = 0;

		/**
		 * Will notify about the current position of the hashing procedure.
		 */
		virtual void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t offset, uint64_t total_size) = 0;
};

class Job
{
	public:
		Job(QuickDC::Hash::Manager* mgr, enum QuickDC::Hash::HashType type, QuickDC::Hash::JobListener* listener, const Samurai::IO::File& file);
		virtual ~Job();

		
		enum QuickDC::Hash::HashType getAlgorithm() const { return algorithm; }

		/**
		 * Hash the whole file, updating 'done' and 'total' as it goes so the
		 * main thread can report progress.
		 *
		 * This runs on the hash worker thread, so it deliberately touches
		 * nothing but this job: no listener callbacks, no cache updates and no
		 * QDBG/QERR (the debug log is a shared unsynchronised file). Failures
		 * are reported through the return value and getError(), and the main
		 * thread does the announcing.
		 *
		 * Returns true when the file was hashed, false on an I/O error.
		 */
		virtual bool runToCompletion(std::atomic<uint64_t>& done, std::atomic<uint64_t>& total);

		/**
		 * errno captured by runToCompletion() when it returned false.
		 */
		int getError() const { return error; }

		const Samurai::IO::File& getFile() const { return file; }
		QuickDC::Hash::JobListener* getListener() const { return listener; }

		virtual const char* getHashSum() const;

		/**
		 * The digest, once the job has produced one, or null before that.
		 *
		 * Held by value: digest() hands out a reference into the hasher, and
		 * this outlives neither a reset() nor the hasher itself.
		 */
		virtual const Samurai::Crypto::Digest::HashValue* getHash() const
		{ return hashValue ? &*hashValue : nullptr; }
		virtual Samurai::Crypto::Digest::Hash* getHasher() { return hasher; }
		
	protected:
		uint8_t* buffer;
		Samurai::IO::File file;
		QuickDC::Hash::JobListener* listener;
		QuickDC::Hash::Manager* mgr;

		off_t offset;
		Samurai::Crypto::Digest::Hash* hasher;
		Samurai::Crypto::Digest::Hash* algo;
		std::optional<Samurai::Crypto::Digest::HashValue> hashValue;
		enum QuickDC::Hash::HashType algorithm;
		int error;

	friend class HashManager;
};

/**
 * A simple job is basically creating a root hash from a set of leaves
 * which is already cached.
 *
 * At this point we have verified to some degree that the actual file is the
 * same as the one cached in this record (size + modification time).
 * Basically, this is all about doing a merkle tree compact.
 */
class SimpleJob : public Job
{
	public:
		SimpleJob(QuickDC::Hash::Manager* mgr, QuickDC::Hash::JobListener* listener, QuickDC::Hash::CacheEntry* cache);
		virtual ~SimpleJob();

		/* The leaves come from the cache, so there is nothing left to hash. */
		virtual bool runToCompletion(std::atomic<uint64_t>& done, std::atomic<uint64_t>& total);

	private:
};

}
}

#endif // HAVE_QUICKDC_HASH_JOB_H
