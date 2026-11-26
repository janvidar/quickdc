/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Hash a real file into a tiger tree, hand the leaves to LeafHashes the way a
 * peer would, and check that it accepts them, agrees on the leaf size, verifies
 * good ranges and rejects bad ones.
 *
 * One case per shape of file, because the shape is what the arithmetic turns on:
 * a file smaller than one leaf, a file that is exactly one, an odd tail, a tree
 * deep enough that a leaf spans many blocks. Every case writes, hashes and
 * removes a scratch file of its own, so nothing is shared between them.
 */

#include "quickdc.h"
#include "network/leafhashes.h"
#include "hash/hashjob.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/crypto/digest/merkletree.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <samurai/posix.h>

/*
 * The scratch file, removed however the case that owns it ends - including the
 * early return an assertion makes.
 */
class Scratch
{
	public:
		explicit Scratch(const char* leaf)
		{
			name = "/tmp/quickdc_leaf_test_" + std::to_string((int) getpid()) + "_";
			for (const char* p = leaf; *p; p++)
				name += (isalnum((unsigned char) *p)) ? *p : '_';
			name += ".dat";
		}

		~Scratch() { Samurai::IO::File(name).remove(); }

		Scratch(const Scratch&) = delete;
		Scratch& operator=(const Scratch&) = delete;

		const std::string& path() const { return name; }

	private:
		std::string name;
};

static bool write_file(const std::string& path, size_t size, unsigned int seed)
{
	Samurai::IO::File file(path);
	if (!file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate))
		return false;

	char* buf = (char*) malloc(size);
	if (!buf) return false;

	for (size_t n = 0; n < size; n++)
	{
		seed = seed * 1103515245u + 12345u;
		buf[n] = (char) ((seed >> 16) & 0xff);
	}
	const bool ok = file.write(buf, size) == (ssize_t) size;
	file.close();
	free(buf);
	return ok;
}

/* Produce root and leaves exactly as the hash manager would. */
static void hash_file(const std::string& path, std::string& tth,
	Samurai::IO::Buffer& leafdata, size_t& leaves, size_t depth)
{
	Samurai::Crypto::Digest::Tiger* algo = new Samurai::Crypto::Digest::Tiger();
	Samurai::Crypto::Digest::MerkleTree tree(algo, 1024, depth);

	Samurai::IO::File file(path);
	file.open(Samurai::IO::File::Mode::Read);

	char buf[65536];
	ssize_t got;
	while ((got = file.read(buf, sizeof(buf))) > 0)
		tree.update((uint8_t*) buf, (uint64_t) got);
	file.close();

	char root[100];
	tree.digest().getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, root, sizeof(root));
	tth = root;

	leaves = tree.countLeaves();
	tree.copyLeavesLTR(leafdata);
	delete algo;
}

/*
 * Everything one shape of file has to satisfy. Returns whether it did; each
 * assertion inside reports its own file, line and expression, and the case that
 * called it is named by the test it is in. Callers wrap it in EXO_ASSERT.
 */
static bool run(const char* label, size_t size, size_t depth = 7)
{
	Scratch scratch(label);
	const std::string& path = scratch.path();

	if (!write_file(path, size, 4711))
	{
		const std::string why = std::string("could not write the scratch file for ")
			+ label;
		EXO_FAIL(why.c_str());
	}

	std::string tth;
	Samurai::IO::Buffer leafdata;
	size_t leaves = 0;
	hash_file(path, tth, leafdata, leaves, depth);

	const size_t length = leafdata.size();
	const std::string data = leafdata.copyRange(0, length);

	QuickDC::LeafHashes lh;
	if (!lh.load(tth.c_str(), size, data.data(), length))
	{
		const std::string why = std::string(label) + ": leaves rejected";
		EXO_FAIL(why.c_str());
	}

	EXO_ASSERT(lh.isLoaded());
	EXO_ASSERT(lh.getLeafCount() == leaves);

	/* The derived leaf size must cover the file in exactly that many leaves. */
	uint64_t ls = lh.getLeafSize();
	EXO_ASSERT(ls > 0);
	EXO_ASSERT((size + ls - 1) / ls == leaves);

	Samurai::IO::File file(path);
	EXO_ASSERT(file.open(Samurai::IO::File::Mode::Read));

	/* Every leaf on its own. */
	for (size_t n = 0; n < leaves; n++)
	{
		uint64_t off = n * ls;
		uint64_t len = (off + ls > size) ? (size - off) : ls;
		if (!lh.verify(file, off, len))
		{
			const std::string why = std::string(label) + ": leaf "
				+ std::to_string(n) + " does not verify on its own";
			EXO_FAIL(why.c_str());
		}
	}

	/* The whole file at once. */
	EXO_ASSERT(lh.verify(file, 0, size));

	/* Unaligned offsets are refused rather than guessed at. */
	if (ls > 1) EXO_ASSERT(!lh.verify(file, 1, ls));
	/* A part-leaf length that is not the tail is refused. */
	if (leaves > 1) EXO_ASSERT(!lh.verify(file, 0, ls / 2));
	/* Past the end. */
	EXO_ASSERT(!lh.verify(file, 0, (uint64_t) size + ls));
	file.close();

	/* Corrupt one byte of the first leaf; that leaf must now fail. */
	{
		Samurai::IO::File w(path);
		w.open(Samurai::IO::File::Mode::Write);
		w.seek(0);
		char bad = 0x2a;
		w.write(&bad, 1);
		w.close();

		Samurai::IO::File r(path);
		r.open(Samurai::IO::File::Mode::Read);
		EXO_ASSERT(!lh.verify(r, 0, (leaves > 1) ? ls : size));
		if (leaves > 1)
		{
			/* ...and a later leaf must still pass. */
			uint64_t off = ls;
			uint64_t len = (off + ls > size) ? (size - off) : ls;
			EXO_ASSERT(lh.verify(r, off, len));
		}
		r.close();
	}

	/* Leaves that do not belong to this file are rejected. */
	{
		QuickDC::LeafHashes other;
		char* twisted = (char*) malloc(length);
		if (!twisted) EXO_FAIL("out of memory twisting the leaf data");
		memcpy(twisted, data.data(), length);
		twisted[0] = (char) (twisted[0] ^ 0xff);
		const bool loaded = other.load(tth.c_str(), size, twisted, length);
		free(twisted);
		EXO_ASSERT(!loaded);
		EXO_ASSERT(!other.isLoaded());
	}

	/* A wrong root is rejected even with genuine leaves. */
	{
		QuickDC::LeafHashes other;
		EXO_ASSERT(!other.load("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", size, data.data(), length));
	}

	/* Truncated leaf data is rejected. */
	if (length > 24)
	{
		QuickDC::LeafHashes other;
		EXO_ASSERT(!other.load(tth.c_str(), size, data.data(), length - 1));
	}

	/* Round trip through the file the download keeps them in. */
	{
		char* fn = QuickDC::LeafHashes::makeFilename(path.c_str());
		EXO_ASSERT(fn != 0);
		EXO_ASSERT(lh.save(fn));

		QuickDC::LeafHashes reloaded;
		EXO_ASSERT(reloaded.load(fn, tth.c_str(), size));
		EXO_ASSERT(reloaded.getLeafCount() == leaves);
		EXO_ASSERT(reloaded.getLeafSize() == ls);

		Samurai::IO::File(fn).remove();
		free(fn);
	}

	return true;
}

/* Smaller than one leaf, so the only leaf is a partial one. */
EXO_TEST(leafhashes_single_leaf,
{
	EXO_ASSERT(run("single leaf", 500));
	return 1;
});

/* And exactly one, where there is no tail at all. */
EXO_TEST(leafhashes_exactly_one_leaf,
{
	EXO_ASSERT(run("exactly one leaf", 1024));
	return 1;
});

EXO_TEST(leafhashes_two_leaves,
{
	EXO_ASSERT(run("two leaves", 2048));
	return 1;
});

/* A last leaf shorter than the rest, which is the length verify() has to work out
   rather than assume. */
EXO_TEST(leafhashes_odd_tail,
{
	EXO_ASSERT(run("odd tail", 5000));
	return 1;
});

EXO_TEST(leafhashes_many_leaves,
{
	EXO_ASSERT(run("many leaves", 300000));
	return 1;
});

EXO_TEST(leafhashes_larger,
{
	EXO_ASSERT(run("larger", 2 * 1024 * 1024));
	return 1;
});

/* Big enough that one leaf spans many blocks, so verifying a leaf has to
   rebuild a subtree rather than hash a single block. */
EXO_TEST(leafhashes_multi_block_leaves,
{
	EXO_ASSERT(run("multi block leaves", 40 * 1024 * 1024));
	return 1;
});

/* The depth we hash our own shares at, so peers get these from us. */
EXO_TEST(leafhashes_deep_tree,
{
	EXO_ASSERT(run("deep tree", 8 * 1024 * 1024, QUICKDC_TTH_TREE_LEVELS));
	return 1;
});

EXO_TEST(leafhashes_deep_tree_odd_size,
{
	EXO_ASSERT(run("deep tree, odd size", 5 * 1024 * 1024 + 777, QUICKDC_TTH_TREE_LEVELS));
	return 1;
});
