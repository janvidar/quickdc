/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <algorithm>
#include "quickdc.h"
#include <stdio.h>

#include "hash/hashcache.h"
#include "hash/hashmanager.h"
#include "hash/hashjob.h"
#include "hash/hashstorage.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/crypto/digest/hash.h>
#include <samurai/crypto/digest/merkletree.h>

/*static*/
QuickDC::Hash::CacheEntry* QuickDC::Hash::CacheEntry::create(const Samurai::IO::File& file, QuickDC::Hash::Job* job)
{
	enum QuickDC::Hash::HashType type = job->getAlgorithm();
	size_t hash_size = getHashSize(type);
	Samurai::Crypto::Digest::MerkleTree* merkle = dynamic_cast<Samurai::Crypto::Digest::MerkleTree*>(job->getHasher());
	size_t leaves, depth;
	
	leaves = merkle ? merkle->countLeaves() : 1;
	Samurai::IO::Buffer leafdata(hash_size * leaves);
	
	if (merkle)
	{
		merkle->copyLeavesLTR(leafdata);
		depth = merkle->getLevels();
	}
	else
	{
		depth = 0;
		const Samurai::Crypto::Digest::HashValue& value = job->getHasher()->digest();
		leafdata.append(reinterpret_cast<const char*>(value.getData()), hash_size);
	}
	
	
	CacheEntry* entry = new CacheEntry(file, type, leafdata, leaves, depth);
	return entry;
}


/*static*/
//QuickDC::Hash::CacheEntry* QuickDC::Hash::CacheEntry::create(const char* fn, time_t mod, off_t size, enum QuickDC::Hash::HashType type, const Samurai::IO::Buffer leafdata, size_t leaves, size_t depth)
QuickDC::Hash::CacheEntry* QuickDC::Hash::CacheEntry::create(CacheEntryHeader& data, const Samurai::IO::Buffer& leafdata)
{
	Samurai::IO::File file(data.name);
	Samurai::TimeStamp modified(data.mtime);
	
	if (!file.exists())
	{
		QERR("File no longer exists (%s)", data.name);
		return 0;
	}
	
	if (file.size() != data.size)
	{
		QERR("File size has changed (%s)", data.name);
		return 0;
	}
	
	if (file.getTimeModified() != modified)
	{
		QERR("File modified time has changed (%s), expected %s, but file is modified at %s", data.name,
			file.getTimeModified().getTime().c_str(), modified.getTime().c_str());
		return 0;
	}
	
	CacheEntry* entry = new CacheEntry(file, static_cast<enum QuickDC::Hash::HashType>(data.algorithm), leafdata, data.leaves, data.depth);
	
	return entry;
}

QuickDC::Hash::CacheEntry::CacheEntry(const Samurai::IO::File& file_, enum QuickDC::Hash::HashType type, const Samurai::IO::Buffer leafdata_, size_t leaves_, size_t depth_)
	: file(file_), size(file_.size()), mtime(file_.getTimeModified()), algorithm(type), leafdata(leafdata_), leaves(leaves_), depth(depth_), cacheFileOffset(0)
{
	QDBG("QuickDC::Hash::CacheEntry::CacheEntry(): file=%s (%zu leaves at %zu bytes)", file.getName().c_str(), leaves, leafdata.size());
}

QuickDC::Hash::CacheEntry::~CacheEntry()
{
}

void QuickDC::Hash::CacheEntry::setCacheFileOffset(off_t pos)
{
	cacheFileOffset = pos;
}

void QuickDC::Hash::CacheEntry::write(Samurai::IO::Buffer* writer)
{
	QDBG("QuickDC::Hash::CacheEntry::write(), file=%s (%zu bytes)", file.getName().c_str(), static_cast<size_t>(file.size()));
	
	CacheRecordHeader header;
	CacheRecordHeader data_header;
	CacheEntryHeader  record;
	
	header.magic = STORAGE_MAGIC_RECORD;
	header.size  = sizeof(record);
	
	data_header.magic = STORAGE_MAGIC_DATA;
	data_header.size = leafdata.size();
	
	const char* filename = file.getName().c_str();

	/* One short of the field: the name is read back as a C string, so a path
	   that fills it exactly leaves nothing to stop at. */
	memset(record.name, 0, sizeof(record.name));
	memcpy(record.name, filename, std::min(strlen(filename), sizeof(record.name) - 1));
	record.size      = size;
	record.mtime     = mtime.getInternalData();
	record.algorithm = static_cast<size_t>(algorithm);
	record.leaves    = leaves;
	record.depth     = depth;

	writer->append(reinterpret_cast<const char*>(&header), sizeof(header));
	writer->append(reinterpret_cast<const char*>(&record), sizeof(record));
	writer->append(reinterpret_cast<const char*>(&data_header), sizeof(data_header));
	writer->append(leafdata);
}

