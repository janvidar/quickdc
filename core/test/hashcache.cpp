/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Hashing a shared file takes as long as reading it, so the leaf hashes are
 * kept in ~/.quickdc/hashdb.cache and read back at startup. What is in there
 * decides what we tell other clients a file's TTH is, and the leaves are served
 * to peers straight out of this file, at an offset the cache records.
 *
 * Two halves, both here: the round trip through Manager, including the checks
 * that stop a file which changed from keeping its old hash, and what happens
 * when the file on disk is not what was written - a half finished save, a bad
 * sector, or a record claiming a length the file does not have.
 */

#include "quickdc.h"
#include "hash/hashmanager.h"
#include "hash/hashcache.h"
#include "hash/hashjob.h"
#include "hash/hashstorage.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>

#include <chrono>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <utime.h>
#include <vector>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* The pid is in the name because a run that dies leaves its files behind, and
   the next one must not find them and take them for its own. */
static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_hashcache_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

static bool writeFile(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) return false;
	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

static std::string readFile(const std::string& path)
{
	FILE* fh = fopen(path.c_str(), "rb");
	if (!fh) return std::string();

	std::string out;
	char buf[65536];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), fh)) > 0) out.append(buf, got);
	fclose(fh);
	return out;
}

/* Counts what the manager tells us, so a cache hit can be told from a job. */
class Listener : public QuickDC::Hash::JobListener
{
	public:
		Listener() : started(0), queued(0), hashed(0) { }

		void EventFileStarted(const QuickDC::Hash::Job*) { started++; }
		void EventFileQueued(const QuickDC::Hash::Job*) { queued++; }
		void EventFileHashed(const QuickDC::Hash::Job*) { hashed++; }
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) { }

		int started;
		int queued;
		int hashed;
};

/* Runs the manager until the worker has nothing left, as the main loop does. */
static bool drain(QuickDC::Hash::Manager& mgr)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (mgr.process() || mgr.isBusy())
	{
		if (std::chrono::steady_clock::now() > deadline) return false;
		usleep(1000);
	}
	mgr.process();
	return true;
}

/*
 * Builds a cache file byte by byte, which is the only way to produce the ones
 * that matter here: the half written and the damaged.
 */
class Handmade
{
	public:
		void master(size_t sizeInfo = sizeof(size_t), size_t version = 1)
		{
			CacheRecordHeader header;
			header.magic = STORAGE_MAGIC_MASTER;
			header.size = sizeof(CacheMasterRecord);

			CacheMasterRecord record;
			record.size_info = sizeInfo;
			record.version = version;

			add(&header, sizeof(header));
			add(&record, sizeof(record));
		}

		/* One complete entry: the record, then its leaf data. */
		void entry(const std::string& name, off_t size, time_t mtime,
			const std::string& leafdata, size_t leaves = 1)
		{
			CacheRecordHeader header;
			header.magic = STORAGE_MAGIC_RECORD;
			header.size = sizeof(CacheEntryHeader);

			CacheEntryHeader record;
			memset(&record, 0, sizeof(record));
			memcpy(record.name, name.c_str(),
				std::min(name.size(), sizeof(record.name) - 1));
			record.size = size;
			record.mtime = mtime;
			record.algorithm = QuickDC::Hash::HashTigerTree;
			record.leaves = leaves;
			record.depth = 1;

			add(&header, sizeof(header));
			add(&record, sizeof(record));
			data(leafdata);
		}

		void data(const std::string& leafdata)
		{
			CacheRecordHeader header;
			header.magic = STORAGE_MAGIC_DATA;
			header.size = (uint32_t) leafdata.size();
			add(&header, sizeof(header));
			out.append(leafdata);
		}

		/* A data header claiming a size the file has not got. */
		void dataClaiming(uint32_t claim, const std::string& actual)
		{
			CacheRecordHeader header;
			header.magic = STORAGE_MAGIC_DATA;
			header.size = claim;
			add(&header, sizeof(header));
			out.append(actual);
		}

		void eof()
		{
			CacheRecordHeader header;
			header.magic = STORAGE_MAGIC_EOF;
			header.size = 0;
			add(&header, sizeof(header));
		}

		void add(const void* p, size_t n) { out.append((const char*) p, n); }

		bool save(const std::string& path) const { return writeFile(path, out); }

		/* Everything up to 'n' bytes, for a save that never finished. */
		bool saveTruncated(const std::string& path, size_t n) const
		{
			return writeFile(path, out.substr(0, std::min(n, out.size())));
		}

		size_t size() const { return out.size(); }

		std::string out;
};

/* A temporary directory of files to share, and a cache of their own. */
class Fixture
{
	public:
		std::string cache;
		std::vector<std::string> files;

		Fixture() : cache(scratch("hashdb.cache")) { }

		~Fixture()
		{
			Samurai::IO::File(cache).remove();
			Samurai::IO::File(cache + ".new").remove();
			for (size_t n = 0; n < files.size(); n++)
				Samurai::IO::File(files[n]).remove();
		}

		Fixture(const Fixture&) = delete;
		Fixture& operator=(const Fixture&) = delete;

		std::string file(const char* leaf, const std::string& content)
		{
			const std::string path = scratch(leaf);
			writeFile(path, content);
			files.push_back(path);
			return path;
		}
};

int main()
{
	setbuf(stdout, 0);

	/* A cache that is not there yet is not an error, just an empty one. */
	{
		Fixture f;
		QuickDC::Hash::Manager mgr(f.cache.c_str());

		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File("/nonexistent"), entry));
		CHECK(!entry);
		printf("  ok a missing cache file loads as an empty cache\n");
	}

	/* Hashing a file puts it in the cache, and the cache reaches the disk. */
	{
		Fixture f;
		const std::string path = f.file("one.bin", std::string(200000, 'a'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;

			mgr.addJob(Samurai::IO::File(path), &listener);
			CHECK(listener.queued == 1);
			CHECK(drain(mgr));
			CHECK(listener.hashed == 1);

			QuickDC::Hash::CacheEntry* entry = 0;
			CHECK(mgr.lookup(Samurai::IO::File(path), entry));
			CHECK(entry && entry->getSize() == 200000);
			CHECK(entry && entry->getLeaves() > 0);
		}

		CHECK(Samurai::IO::File(f.cache).exists());
		printf("  ok a hashed file is remembered and written out\n");
	}

	/*
	 * And is read back rather than hashed again. A cache hit answers straight
	 * away, without the job ever being queued.
	 */
	{
		Fixture f;
		const std::string path = f.file("two.bin", std::string(300000, 'b'));

		size_t leafBytes = 0;
		size_t leaves = 0;

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			CHECK(drain(mgr));

			QuickDC::Hash::CacheEntry* entry = 0;
			CHECK(mgr.lookup(Samurai::IO::File(path), entry));
			if (entry)
			{
				leafBytes = entry->getLeafdataSize();
				leaves = entry->getLeaves();
			}
		}

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		Listener listener;

		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(mgr.lookup(Samurai::IO::File(path), entry));
		CHECK(entry && entry->getLeafdataSize() == leafBytes);
		CHECK(entry && entry->getLeaves() == leaves);

		mgr.addJob(Samurai::IO::File(path), &listener);
		CHECK(listener.hashed == 1);
		CHECK(listener.queued == 0);
		printf("  ok a cached file is answered from the cache, not hashed again\n");
	}

	/*
	 * The leaves are served to peers out of the cache file itself, so the
	 * offset the entry remembers has to point at them.
	 */
	{
		Fixture f;
		const std::string path = f.file("three.bin", std::string(400000, 'c'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			CHECK(drain(mgr));
		}

		QuickDC::Hash::Manager mgr(f.cache.c_str());

		uint64_t offset = 0, length = 0;
		std::unique_ptr<Samurai::IO::File> cachefile(
			mgr.getLeafData(Samurai::IO::File(path), offset, length));

		CHECK(cachefile);
		CHECK(length > 0);

		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(mgr.lookup(Samurai::IO::File(path), entry));

		if (cachefile && entry)
		{
			const std::string raw = readFile(f.cache);
			CHECK(offset + length <= raw.size());

			if (offset + length <= raw.size())
			{
				const std::string onDisk = raw.substr(offset, length);
				Samurai::IO::Buffer& leaves = entry->getLeafdata();
				CHECK(onDisk == leaves.copyRange(0, leaves.size()));
			}
		}
		printf("  ok the leaf data sits where the entry says it does\n");
	}

	/* Every cache file ends with the record that says so. */
	{
		Fixture f;
		const std::string path = f.file("four.bin", std::string(100000, 'd'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			CHECK(drain(mgr));
		}

		const std::string raw = readFile(f.cache);
		CHECK(raw.size() > sizeof(CacheRecordHeader));

		CacheRecordHeader tail;
		memset(&tail, 0, sizeof(tail));
		if (raw.size() >= sizeof(tail))
			memcpy(&tail, &raw[raw.size() - sizeof(tail)], sizeof(tail));

		CHECK(tail.magic == STORAGE_MAGIC_EOF);
		CHECK(tail.size == 0);
		printf("  ok the cache file ends with an end of file record\n");
	}

	/* Several files all come back. */
	{
		Fixture f;
		const std::string a = f.file("a.bin", std::string(50000, 'x'));
		const std::string b = f.file("b.bin", std::string(60000, 'y'));
		const std::string c = f.file("c.bin", std::string(70000, 'z'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(a), &listener);
			mgr.addJob(Samurai::IO::File(b), &listener);
			mgr.addJob(Samurai::IO::File(c), &listener);
			CHECK(drain(mgr));
			CHECK(listener.hashed == 3);
		}

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(mgr.lookup(Samurai::IO::File(a), entry));
		CHECK(mgr.lookup(Samurai::IO::File(b), entry));
		CHECK(mgr.lookup(Samurai::IO::File(c), entry));
		printf("  ok several files round trip through one cache\n");
	}

	/* A file that has been replaced since is not what we hashed. */
	{
		Fixture f;
		const std::string path = f.file("changed.bin", std::string(100000, 'e'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			CHECK(drain(mgr));
		}

		/* Same name, different contents and length. */
		writeFile(path, std::string(120000, 'f'));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File(path), entry));
		printf("  ok a file whose size changed does not keep its old hash\n");
	}

	/* Nor one that was touched without changing length. */
	{
		Fixture f;
		const std::string path = f.file("touched.bin", std::string(100000, 'g'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			CHECK(drain(mgr));
		}

		struct utimbuf times;
		times.actime = time(0) + 3600;
		times.modtime = time(0) + 3600;
		CHECK(utime(path.c_str(), &times) == 0);

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File(path), entry));
		printf("  ok a file whose timestamp changed does not keep its hash\n");
	}

	/* Nor one that is gone. */
	{
		Fixture f;
		const std::string path = f.file("gone.bin", std::string(100000, 'h'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			CHECK(drain(mgr));
		}

		Samurai::IO::File(path).remove();

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File(path), entry));
		printf("  ok a file that is gone does not stay in the cache\n");
	}

	/* A hand made cache is read the same way as one we wrote. */
	{
		Fixture f;
		const std::string path = f.file("hand.bin", "the contents do not matter here");

		Samurai::IO::File probe(path);
		Handmade made;
		made.master();
		made.entry(path, probe.size(), probe.getTimeModified().getInternalData(),
			std::string(24, '\x41'));
		made.eof();
		CHECK(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(mgr.lookup(Samurai::IO::File(path), entry));
		CHECK(entry && entry->getLeafdataSize() == 24);
		CHECK(entry && entry->getSize() == probe.size());
		printf("  ok a hand made cache file reads back\n");
	}

	/* An empty file is not a cache. */
	{
		Fixture f;
		CHECK(writeFile(f.cache, ""));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File("/nonexistent"), entry));
		printf("  ok an empty cache file is refused\n");
	}

	/* Neither is something else entirely. */
	{
		Fixture f;
		CHECK(writeFile(f.cache, std::string(4096, '?')));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File("/nonexistent"), entry));
		printf("  ok a cache file that is not one is refused\n");
	}

	/* A cache written by a version that is not this one. */
	{
		Fixture f;
		const std::string path = f.file("version.bin", "x");

		Samurai::IO::File probe(path);
		Handmade made;
		made.master(sizeof(size_t), 99);
		made.entry(path, probe.size(), probe.getTimeModified().getInternalData(),
			std::string(24, '\x42'));
		made.eof();
		CHECK(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File(path), entry));
		printf("  ok a cache from another version is refused whole\n");
	}

	/* And one from a machine whose size_t is a different width. */
	{
		Fixture f;
		const std::string path = f.file("width.bin", "x");

		Samurai::IO::File probe(path);
		Handmade made;
		made.master(sizeof(size_t) == 8 ? 4 : 8, 1);
		made.entry(path, probe.size(), probe.getTimeModified().getInternalData(),
			std::string(24, '\x43'));
		made.eof();
		CHECK(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File(path), entry));
		printf("  ok a cache from a machine of another width is refused\n");
	}

	/*
	 * A save that was interrupted. Everything written before the cut has to
	 * survive: it is the difference between rehashing one file and rehashing
	 * the whole share.
	 */
	{
		Fixture f;
		const std::string a = f.file("keep.bin", "first file");
		const std::string b = f.file("lose.bin", "second file");

		Samurai::IO::File pa(a), pb(b);
		Handmade made;
		made.master();
		made.entry(a, pa.size(), pa.getTimeModified().getInternalData(), std::string(24, '\x44'));
		const size_t afterFirst = made.size();
		made.entry(b, pb.size(), pb.getTimeModified().getInternalData(), std::string(24, '\x45'));
		made.eof();

		/* Cut in the middle of the second record. */
		CHECK(made.saveTruncated(f.cache, afterFirst + 20));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(mgr.lookup(Samurai::IO::File(a), entry));
		CHECK(!mgr.lookup(Samurai::IO::File(b), entry));
		printf("  ok a half written cache keeps the entries that made it\n");
	}

	/* Cut between two whole entries, with no end of file record. */
	{
		Fixture f;
		const std::string a = f.file("nofin.bin", "only file");

		Samurai::IO::File pa(a);
		Handmade made;
		made.master();
		made.entry(a, pa.size(), pa.getTimeModified().getInternalData(), std::string(24, '\x46'));
		CHECK(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(mgr.lookup(Samurai::IO::File(a), entry));
		printf("  ok a cache with no end of file record still reads\n");
	}

	/*
	 * A record claiming more leaf data than the whole file holds. The claim is
	 * a uint32, and the read buffered all of it before finding out the file was
	 * nowhere near that long - four gigabytes of it, for a cache file whose
	 * last sector went bad.
	 */
	{
		Fixture f;
		const std::string a = f.file("huge.bin", "small file");

		Samurai::IO::File pa(a);
		Handmade made;
		made.master();

		CacheRecordHeader header;
		header.magic = STORAGE_MAGIC_RECORD;
		header.size = sizeof(CacheEntryHeader);

		CacheEntryHeader record;
		memset(&record, 0, sizeof(record));
		memcpy(record.name, a.c_str(), std::min(a.size(), sizeof(record.name) - 1));
		record.size = pa.size();
		record.mtime = pa.getTimeModified().getInternalData();
		record.algorithm = QuickDC::Hash::HashTigerTree;
		record.leaves = 1;
		record.depth = 1;

		made.add(&header, sizeof(header));
		made.add(&record, sizeof(record));
		made.dataClaiming(0xfffffff0u, std::string(24, '\x47'));
		CHECK(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(!mgr.lookup(Samurai::IO::File(a), entry));
		printf("  ok a record claiming more than the file holds is refused\n");
	}

	/* A record whose magic is not one we know stops the read there. */
	{
		Fixture f;
		const std::string a = f.file("badmagic.bin", "first");
		const std::string b = f.file("after.bin", "second");

		Samurai::IO::File pa(a), pb(b);
		Handmade made;
		made.master();
		made.entry(a, pa.size(), pa.getTimeModified().getInternalData(), std::string(24, '\x48'));

		CacheRecordHeader bogus;
		bogus.magic = 0xdeadbeef;
		bogus.size = sizeof(CacheEntryHeader);
		made.add(&bogus, sizeof(bogus));

		made.entry(b, pb.size(), pb.getTimeModified().getInternalData(), std::string(24, '\x49'));
		made.eof();
		CHECK(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		CHECK(mgr.lookup(Samurai::IO::File(a), entry));
		printf("  ok an unknown record stops the read rather than the process\n");
	}

	/* Rewriting the cache replaces it rather than growing it. */
	{
		Fixture f;
		const std::string a = f.file("rewrite.bin", std::string(80000, 'i'));

		size_t first = 0;
		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(a), &listener);
			CHECK(drain(mgr));
		}
		first = readFile(f.cache).size();
		CHECK(first > 0);

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(a), &listener);
			CHECK(drain(mgr));
		}
		CHECK(readFile(f.cache).size() == first);
		printf("  ok saving the cache again replaces it\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
