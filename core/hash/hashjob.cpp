/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "api/core.h"
#include "hash/hashjob.h"
#include "hash/hashmanager.h"
#include "hash/hashcache.h"
#include "share/sharemanager.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <errno.h>

#include <samurai/crypto/digest/hash.h>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/crypto/digest/merkletree.h>


QuickDC::Hash::Job::Job(QuickDC::Hash::Manager* mgr_, enum QuickDC::Hash::HashType type, QuickDC::Hash::JobListener* listener_, const Samurai::IO::File& file_) : buffer(0), file(file_), listener(listener_), mgr(mgr_), offset(0), hasher(0), algo(0), hashValue(0), algorithm(type)
{
	switch (algorithm)
	{
		case HashTiger:
		{
			hasher = new Samurai::Crypto::Digest::Tiger();
			break;
		}
		case HashTigerTree:
		{
			algo = new Samurai::Crypto::Digest::Tiger();
			hasher = new Samurai::Crypto::Digest::MerkleTree(algo);
			break;
		}
		case HashMD5:
		{
			// hasher = new Samurai::Crypto::Digest::MD5();
			break;
		}
		case HashSHA1:
		{
			// hasher = new Samurai::Crypto::Digest::SHA1();
			break;
		}
		case HashSHA256:
		{
			// hasher = new Samurai::Crypto::Digest::SHA256();
			break;
		}
	}

	QDBG("Created hasjob, file='%s'", file.getName().c_str());
}

QuickDC::Hash::SimpleJob::SimpleJob(QuickDC::Hash::Manager* mgr, QuickDC::Hash::JobListener* listener, QuickDC::Hash::CacheEntry* cache) : QuickDC::Hash::Job(mgr, cache->getAlgorithm(), listener, cache->getFile())
{
	Samurai::Crypto::Digest::MerkleTree* merkle = dynamic_cast<Samurai::Crypto::Digest::MerkleTree*>(hasher);
	assert(merkle != NULL);
	
	merkle->setLeavesLTR(cache->getLeafdata(), cache->getLeaves(), (uint64_t) cache->getSize());
	hashValue = merkle->digest();
}

QuickDC::Hash::SimpleJob::~SimpleJob()
{
}

ssize_t QuickDC::Hash::SimpleJob::process()
{
	return 0;
}


QuickDC::Hash::Job::~Job()
{
	file.close();
	delete[] buffer;
	delete hasher;
	delete algo;
}

#define DEFAULT_HASH_BUFFER_SIZE 1024*1024
ssize_t QuickDC::Hash::Job::process(/*size_t chunk_size*/)
{
	const size_t chunk_size = DEFAULT_HASH_BUFFER_SIZE;	

	if (!buffer)
	{
		if (file.open(Samurai::IO::File::Read))
		{
			QDBG("QuickDC::Hash::Job::start(), file='%s'", file.getName().c_str());
			listener->EventFileStarted(this);
			buffer = new uint8_t[chunk_size];
		}
		else
		{
			return -1;
		}
	}
	
	ssize_t sz = file.read((char*) buffer, chunk_size);
	
	if (sz > 0)
	{
		hasher->update((uint8_t*) buffer, sz);
		listener->EventFileHashProgress(this, offset + sz, file.size());
	}
	else if (sz == 0)
	{
		hashValue = hasher->digest();
		
		if (mgr)
		{
			QDBG("QuickDC::Hash::Job::process() - finished, file='%s'", file.getName().c_str());
			QuickDC::Hash::CacheEntry* entry = QuickDC::Hash::CacheEntry::create(file, this);
			mgr->addCache(file.getName().c_str(), entry);
		}
		listener->EventFileHashed(this);
		delete[] buffer; buffer = 0;
	}
	else
	{
		QERR("HashJob::hash_chunk(): (%s) File reading failed. %d: %s.", file.getName().c_str(), errno, strerror(errno));
		delete[] buffer; buffer = 0;
	}
	return sz;
}

const char* QuickDC::Hash::Job::getHashSum() const
{
	static char buf[100];
	if (hashValue->getFormattedString(Samurai::Crypto::Digest::HashValue::FormatBase32, buf, 100))
		return buf;
	return "";
}
