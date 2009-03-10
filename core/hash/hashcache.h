/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HASH_CACHE_H
#define HAVE_QUICKDC_HASH_CACHE_H

#include "quickdc.h"
#include "hash/hashmanager.h"
#include "hash/hashstorage.h"
#include <string>
#include <vector>
#include <map>

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/crypto/digest/hash.h>

namespace QuickDC {
namespace Hash {

class CacheStorage;
class CacheEntry
{
	public:
		static CacheEntry* create(const Samurai::IO::File&, QuickDC::Hash::Job* job);
		static CacheEntry* create(CacheEntryHeader& data, const Samurai::IO::Buffer& leafdata);
	
		const Samurai::IO::File& getFile() const { return file; }
		off_t getSize() const { return size; }
		size_t getLeaves() const { return leaves; }
		size_t getDepth() const { return depth; }
		Samurai::IO::Buffer& getLeafdata() { return leafdata; }
		enum QuickDC::Hash::HashType getAlgorithm() const { return algorithm; }
		
		void setCacheFileOffset(off_t pos);
		off_t getCacheFileOffset() const { return cacheFileOffset; }
		size_t getLeafdataSize() const { return leafdata.size(); }
		
		
	protected:
		CacheEntry(const Samurai::IO::File&, QuickDC::Hash::HashType, const Samurai::IO::Buffer, size_t, size_t);
		virtual ~CacheEntry();
		
	protected:
		void write(Samurai::IO::Buffer* writer);
		
	protected:
		Samurai::IO::File file;
		off_t size;
		Samurai::TimeStamp mtime;
		enum QuickDC::Hash::HashType algorithm;
		Samurai::IO::Buffer leafdata;
		size_t leaves;
		size_t depth;

		// optional, only when requested
		Samurai::Crypto::Digest::HashValue* digest;
		
		// may not be ready
		off_t cacheFileOffset;

		
	friend class Manager;
	friend class CacheStorage;
};

}
}

#endif // HAVE_QUICKDC_HASH_CACHE_H

