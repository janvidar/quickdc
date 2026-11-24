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


QuickDC::Hash::Job::Job(QuickDC::Hash::Manager* mgr_, enum QuickDC::Hash::HashType type, QuickDC::Hash::JobListener* listener_, const Samurai::IO::File& file_) : buffer(0), file(file_), listener(listener_), mgr(mgr_), offset(0), hasher(0), algo(0), algorithm(type), error(0)
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
			hasher = new Samurai::Crypto::Digest::MerkleTree(algo, 1024, QUICKDC_TTH_TREE_LEVELS);
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
	
	merkle->setLeavesLTR(cache->getLeafdata(), cache->getLeaves(), static_cast<uint64_t>(cache->getSize()));
	hashValue = merkle->digest();
}

QuickDC::Hash::SimpleJob::~SimpleJob()
{
}

bool QuickDC::Hash::SimpleJob::runToCompletion(std::atomic<uint64_t>& done, std::atomic<uint64_t>& total)
{
	total.store(0);
	done.store(0);
	return true;
}


QuickDC::Hash::Job::~Job()
{
	file.close();
	delete[] buffer;
	delete hasher;
	delete algo;
}

#define DEFAULT_HASH_BUFFER_SIZE 1024*1024

/**
 * Runs on the hash worker thread - see the contract in hashjob.h. Everything
 * touched here (file, buffer, hasher, hashValue, error) belongs to this job
 * alone, and the two atomics are the only things the main thread reads while
 * this is running.
 */
bool QuickDC::Hash::Job::runToCompletion(std::atomic<uint64_t>& done, std::atomic<uint64_t>& total)
{
	const size_t chunk_size = DEFAULT_HASH_BUFFER_SIZE;

	error = 0;
	done.store(0);
	total.store(0);

	if (!file.open(Samurai::IO::File::Mode::Read))
	{
		error = errno;
		return false;
	}

	total.store(static_cast<uint64_t>(file.size()));
	buffer = new uint8_t[chunk_size];

	for (;;)
	{
		ssize_t sz = file.read(reinterpret_cast<char*>(buffer), chunk_size);

		if (sz > 0)
		{
			hasher->update(buffer, sz);
			offset += sz;
			done.store(static_cast<uint64_t>(offset));
		}
		else if (sz == 0)
		{
			hashValue = hasher->digest();
			break;
		}
		else
		{
			error = errno;
			delete[] buffer; buffer = 0;
			return false;
		}
	}

	delete[] buffer; buffer = 0;
	return true;
}

const char* QuickDC::Hash::Job::getHashSum() const
{
	thread_local char buf[100];
	if (!hashValue) return "";

	if (hashValue->getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, buf, 100))
		return buf;
	return "";
}
