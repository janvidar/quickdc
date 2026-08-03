/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/leafhashes.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/crypto/digest/merkletree.h>

#include <string.h>
#include <string_view>

/* The smallest leaf a tiger tree uses; every larger one doubles it. */
#define TTH_MIN_LEAF_SIZE 1024

QuickDC::LeafHashes::LeafHashes()
	: leafCount(0)
	, hashSize(0)
	, leafSize(0)
	, fileSize(0)
{
}

QuickDC::LeafHashes::~LeafHashes()
{
}

/**
 * The leaf size is not sent, only implied: it is the smallest one that covers
 * the file in the number of leaves we were given.
 */
static uint64_t leaf_size_for(uint64_t fileSize, size_t leafCount)
{
	if (!leafCount) return 0;
	if (!fileSize) return TTH_MIN_LEAF_SIZE;

	for (uint64_t size = TTH_MIN_LEAF_SIZE; size; size *= 2)
	{
		uint64_t needed = (fileSize + size - 1) / size;
		if (needed == static_cast<uint64_t>(leafCount)) return size;
		if (needed <= 1) break;
	}
	return 0;
}

bool QuickDC::LeafHashes::load(const char* tth, uint64_t fileSize_, const char* data, size_t length)
{
	if (!tth || !data || !length || !fileSize_) return false;

	Samurai::Crypto::Digest::Tiger tiger;
	size_t size = tiger.size();
	if (!size || (length % size)) return false;

	size_t count = length / size;
	uint64_t leaf = leaf_size_for(fileSize_, count);
	if (!leaf)
	{
		QDBG("Leaf data for '%s' does not describe a %d byte file", tth, static_cast<int>(fileSize_));
		return false;
	}

	/* Hash the leaves back into a root; it has to be the one we asked for. */
	Samurai::IO::Buffer buffer;
	buffer.append(data, length);

	Samurai::Crypto::Digest::Tiger* hasher = new Samurai::Crypto::Digest::Tiger();
	Samurai::Crypto::Digest::MerkleTree tree(hasher);
	tree.setLeavesLTR(buffer, count, fileSize_);

	char root[100];
	const Samurai::Crypto::Digest::HashValue& digest = tree.digest();
	/* 'root' is only a string once getFormattedString() says it wrote one. */
	bool ok = digest.getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, root, sizeof(root))
		&& std::string_view(root) == tth;

	delete hasher;

	if (!ok)
	{
		QDBG("Leaf data does not hash up to '%s'; discarding it", tth);
		return false;
	}

	leaves.assign(data, data + length);
	leafCount = count;
	hashSize = size;
	leafSize = leaf;
	fileSize = fileSize_;

	QDBG("Got %d verified leaves of %d bytes each for '%s'", static_cast<int>(leafCount), static_cast<int>(leafSize), tth);
	return true;
}

bool QuickDC::LeafHashes::verify(Samurai::IO::File& file, uint64_t offset, uint64_t length) const
{
	if (leaves.empty() || !length) return false;
	if (offset % leafSize) return false;
	if (offset + length > fileSize) return false;

	/* Whole leaves, unless this is the tail of the file. */
	if ((length % leafSize) && (offset + length != fileSize)) return false;

	size_t first = static_cast<size_t>(offset / leafSize);
	char buffer[65536];
	bool ok = true;

	for (uint64_t at = offset; ok && at < offset + length; at += leafSize)
	{
		uint64_t want = leafSize;
		if (at + want > fileSize) want = fileSize - at;

		/*
		 * A leaf covering more than one block is the root of the tree over
		 * those blocks, not a hash of the whole span, so let a tree do it.
		 */
		Samurai::Crypto::Digest::Tiger* algo = new Samurai::Crypto::Digest::Tiger();
		Samurai::Crypto::Digest::MerkleTree leaf(algo);

		if (!file.seek(static_cast<off_t>(at))) ok = false;

		for (uint64_t done = 0; ok && done < want; )
		{
			size_t piece = sizeof(buffer);
			if (done + piece > want) piece = static_cast<size_t>(want - done);

			if (file.read(buffer, piece) != static_cast<ssize_t>(piece))
			{
				QERR("Unable to read %d bytes at %d of '%s' to verify it",
					static_cast<int>(piece), static_cast<int>(at + done), file.getName().c_str());
				ok = false;
				break;
			}

			leaf.update(buffer, piece);
			done += piece;
		}

		if (ok)
		{
			const Samurai::Crypto::Digest::HashValue& value = leaf.digest();
			size_t index = first + static_cast<size_t>((at - offset) / leafSize);

			if (index >= leafCount
				|| memcmp(value.getData(), leaves.data() + index * hashSize, hashSize))
			{
				QDBG("Leaf %d of '%s' does not match", static_cast<int>(index), file.getName().c_str());
				ok = false;
			}
		}

		delete algo;
	}

	return ok;
}

char* QuickDC::LeafHashes::makeFilename(const char* tempFilename)
{
	if (!tempFilename) return 0;

	size_t length = strlen(tempFilename) + 6;
	char* out = static_cast<char*>(malloc(length));
	if (out) snprintf(out, length, "%s.tthl", tempFilename);
	return out;
}

bool QuickDC::LeafHashes::save(const char* filename) const
{
	if (leaves.empty() || !filename) return false;

	Samurai::IO::File file(filename);
	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate)) return false;

	bool ok = (file.write(leaves.data(), leafCount * hashSize) == static_cast<ssize_t>(leafCount * hashSize));
	file.close();
	return ok;
}

bool QuickDC::LeafHashes::load(const char* filename, const char* tth, uint64_t fileSize_)
{
	if (!filename) return false;

	Samurai::IO::File file(filename);
	if (!file.exists() || !file.open(Samurai::IO::File::Mode::Read)) return false;

	size_t length = static_cast<size_t>(file.size());
	if (!length)
	{
		file.close();
		return false;
	}

	std::vector<char> data(length);
	bool ok = (file.read(data.data(), length) == static_cast<ssize_t>(length));
	file.close();

	if (ok) ok = load(tth, fileSize_, data.data(), length);
	return ok;
}
