/*
 * Hash a real file into a tiger tree, hand the leaves to LeafHashes the way a
 * peer would, and check that it accepts them, agrees on the leaf size, verifies
 * good ranges and rejects bad ones.
 */

#include "quickdc.h"
#include "network/leafhashes.h"
#include "hash/hashjob.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/crypto/digest/merkletree.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr) \
	do { \
		if (!(expr)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } \
	} while (0)

static std::string path;

static void write_file(size_t size, unsigned int seed)
{
	Samurai::IO::File file(path);
	file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate);
	char* buf = (char*) malloc(size);
	for (size_t n = 0; n < size; n++)
	{
		seed = seed * 1103515245u + 12345u;
		buf[n] = (char) ((seed >> 16) & 0xff);
	}
	file.write(buf, size);
	file.close();
	free(buf);
}

/* Produce root and leaves exactly as the hash manager would. */
static void hash_file(std::string& tth, Samurai::IO::Buffer& leafdata, size_t& leaves, size_t depth)
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
	tree.digest()->getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, root, sizeof(root));
	tth = root;

	leaves = tree.countLeaves();
	tree.copyLeavesLTR(leafdata);
	delete algo;
}

static void run(const char* label, size_t size, size_t depth = 7)
{
	write_file(size, 4711);

	std::string tth;
	Samurai::IO::Buffer leafdata;
	size_t leaves = 0;
	hash_file(tth, leafdata, leaves, depth);

	const size_t length = leafdata.size();
	const std::string data = leafdata.copyRange(0, length);

	QuickDC::LeafHashes lh;
	if (!lh.load(tth.c_str(), size, data.data(), length))
	{
		printf("FAIL %s: leaves rejected\n", label);
		failures++;
		return;
	}

	CHECK(lh.isLoaded());
	CHECK(lh.getLeafCount() == leaves);

	/* The derived leaf size must cover the file in exactly that many leaves. */
	uint64_t ls = lh.getLeafSize();
	CHECK(ls > 0);
	CHECK((size + ls - 1) / ls == leaves);

	Samurai::IO::File file(path);
	CHECK(file.open(Samurai::IO::File::Mode::Read));

	/* Every leaf on its own. */
	for (size_t n = 0; n < leaves; n++)
	{
		uint64_t off = n * ls;
		uint64_t len = (off + ls > size) ? (size - off) : ls;
		CHECK(lh.verify(file, off, len));
	}

	/* The whole file at once. */
	CHECK(lh.verify(file, 0, size));

	/* Unaligned offsets are refused rather than guessed at. */
	if (ls > 1) CHECK(!lh.verify(file, 1, ls));
	/* A part-leaf length that is not the tail is refused. */
	if (leaves > 1) CHECK(!lh.verify(file, 0, ls / 2));
	/* Past the end. */
	CHECK(!lh.verify(file, 0, (uint64_t) size + ls));
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
		CHECK(!lh.verify(r, 0, (leaves > 1) ? ls : size));
		if (leaves > 1)
		{
			/* ...and a later leaf must still pass. */
			uint64_t off = ls;
			uint64_t len = (off + ls > size) ? (size - off) : ls;
			CHECK(lh.verify(r, off, len));
		}
		r.close();
	}

	/* Leaves that do not belong to this file are rejected. */
	{
		QuickDC::LeafHashes other;
		char* twisted = (char*) malloc(length);
		memcpy(twisted, data.data(), length);
		twisted[0] = (char) (twisted[0] ^ 0xff);
		CHECK(!other.load(tth.c_str(), size, twisted, length));
		CHECK(!other.isLoaded());
		free(twisted);
	}

	/* A wrong root is rejected even with genuine leaves. */
	{
		QuickDC::LeafHashes other;
		CHECK(!other.load("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", size, data.data(), length));
	}

	/* Truncated leaf data is rejected. */
	if (length > 24)
	{
		QuickDC::LeafHashes other;
		CHECK(!other.load(tth.c_str(), size, data.data(), length - 1));
	}

	/* Round trip through the file the download keeps them in. */
	{
		char* fn = QuickDC::LeafHashes::makeFilename(path.c_str());
		CHECK(fn != 0);
		CHECK(lh.save(fn));

		QuickDC::LeafHashes reloaded;
		CHECK(reloaded.load(fn, tth.c_str(), size));
		CHECK(reloaded.getLeafCount() == leaves);
		CHECK(reloaded.getLeafSize() == ls);

		Samurai::IO::File(fn).remove();
		free(fn);
	}

	printf("  ok %-22s %9zu bytes, %3zu leaves of %6d\n", label, size, leaves, (int) ls);
}

int main(int argc, char** argv)
{
	setbuf(stdout, 0);
	const char* dir = (argc > 1) ? argv[1] : "/tmp";
	path = std::string(dir) + "/quickdc_leaf_test.dat";

	run("single leaf", 500);
	run("exactly one leaf", 1024);
	run("two leaves", 2048);
	run("odd tail", 5000);
	run("many leaves", 300000);
	run("larger", 2 * 1024 * 1024);

	/* Big enough that one leaf spans many blocks, so verifying a leaf has to
	   rebuild a subtree rather than hash a single block. */
	run("multi block leaves", 40 * 1024 * 1024);

	/* The depth we hash our own shares at, so peers get these from us. */
	run("deep tree", 8 * 1024 * 1024, QUICKDC_TTH_TREE_LEVELS);
	run("deep tree, odd size", 5 * 1024 * 1024 + 777, QUICKDC_TTH_TREE_LEVELS);

	Samurai::IO::File(path).remove();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
