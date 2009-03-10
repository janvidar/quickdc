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

#include <samurai/io/file.h>
#include <samurai/crypto/digest/hash.h>
#include <samurai/crypto/digest/tigertree.h>

namespace Samurai {
namespace IO {
	class File;
	class Buffer;
}
}

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
		 * Partial hash, 'chunk_size' bytes from 'offset' 
		 * return >0 if more is to be hash.
		 * return =0 if no more to hash (successfull end).
		 * return <0 if an error occured.
		 */
		virtual ssize_t process();

		virtual const char* getHashSum() const;
		virtual Samurai::Crypto::Digest::HashValue* getHash() const { return hashValue; }
		virtual Samurai::Crypto::Digest::Hash* getHasher() { return hasher; }
		
	protected:
		uint8_t* buffer;
		Samurai::IO::File file;
		QuickDC::Hash::JobListener* listener;
		QuickDC::Hash::Manager* mgr;

		off_t offset;
		Samurai::Crypto::Digest::Hash* hasher;
		Samurai::Crypto::Digest::Hash* algo;
		Samurai::Crypto::Digest::HashValue* hashValue;
		enum QuickDC::Hash::HashType algorithm;
		
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
	
		virtual ssize_t process();
		
	private:
};

}
}

#endif // HAVE_QUICKDC_HASH_JOB_H
