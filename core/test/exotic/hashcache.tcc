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

#include <thread>
#include <samurai/io/file.h>
#include <samurai/io/buffer.h>

#include <chrono>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <samurai/posix.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif
#include <vector>

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
		Listener() : started(0), queued(0), hashed(0), progress(0) { }

		void EventFileStarted(const QuickDC::Hash::Job*) override { started++; }
		void EventFileQueued(const QuickDC::Hash::Job*) override { queued++; }
		void EventFileHashed(const QuickDC::Hash::Job*) override { hashed++; }
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) override { progress++; }

		int started;
		int queued;
		int hashed;
		int progress;
};

/* Runs the manager until the worker has nothing left, as the main loop does. */
static bool drain(QuickDC::Hash::Manager& mgr)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (mgr.process() || mgr.isBusy())
	{
		if (std::chrono::steady_clock::now() > deadline) return false;
		std::this_thread::sleep_for(std::chrono::microseconds(1000));
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

/* A cache that is not there yet is not an error, just an empty one. */
EXO_TEST(hashcache_a_missing_cache_file_loads_as_an_empty_cache,
{
		Fixture f;
		QuickDC::Hash::Manager mgr(f.cache.c_str());

		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File("/nonexistent"), entry));
		EXO_ASSERT(!entry);
	
	return 1;
});

/* Hashing a file puts it in the cache, and the cache reaches the disk. */
EXO_TEST(hashcache_a_hashed_file_is_remembered_and_written_out,
{
		Fixture f;
		const std::string path = f.file("one.bin", std::string(200000, 'a'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;

			mgr.addJob(Samurai::IO::File(path), &listener);
			EXO_ASSERT(listener.queued == 1);
			EXO_ASSERT(drain(mgr));
			EXO_ASSERT(listener.hashed == 1);

			QuickDC::Hash::CacheEntry* entry = 0;
			EXO_ASSERT(mgr.lookup(Samurai::IO::File(path), entry));
			EXO_ASSERT(entry && entry->getSize() == 200000);
			EXO_ASSERT(entry && entry->getLeaves() > 0);
		}

		EXO_ASSERT(Samurai::IO::File(f.cache).exists());
	
	return 1;
});

/*
	 * And is read back rather than hashed again. A cache hit answers straight
	 * away, without the job ever being queued.
	 */
EXO_TEST(hashcache_a_cached_file_is_answered_from_the_cache_not_hashed_again,
{
		Fixture f;
		const std::string path = f.file("two.bin", std::string(300000, 'b'));

		size_t leafBytes = 0;
		size_t leaves = 0;

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			EXO_ASSERT(drain(mgr));

			QuickDC::Hash::CacheEntry* entry = 0;
			EXO_ASSERT(mgr.lookup(Samurai::IO::File(path), entry));
			if (entry)
			{
				leafBytes = entry->getLeafdataSize();
				leaves = entry->getLeaves();
			}
		}

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		Listener listener;

		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(path), entry));
		EXO_ASSERT(entry && entry->getLeafdataSize() == leafBytes);
		EXO_ASSERT(entry && entry->getLeaves() == leaves);

		mgr.addJob(Samurai::IO::File(path), &listener);
		EXO_ASSERT(listener.hashed == 1);
		EXO_ASSERT(listener.queued == 0);
	
	return 1;
});

/*
	 * The leaves are served to peers out of the cache file itself, so the
	 * offset the entry remembers has to point at them.
	 */
EXO_TEST(hashcache_the_leaf_data_sits_where_the_entry_says_it_does,
{
		Fixture f;
		const std::string path = f.file("three.bin", std::string(400000, 'c'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			EXO_ASSERT(drain(mgr));
		}

		QuickDC::Hash::Manager mgr(f.cache.c_str());

		uint64_t offset = 0, length = 0;
		std::unique_ptr<Samurai::IO::File> cachefile(
			mgr.getLeafData(Samurai::IO::File(path), offset, length));

		EXO_ASSERT(cachefile);
		EXO_ASSERT(length > 0);

		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(path), entry));

		if (cachefile && entry)
		{
			const std::string raw = readFile(f.cache);
			EXO_ASSERT(offset + length <= raw.size());

			if (offset + length <= raw.size())
			{
				const std::string onDisk = raw.substr(offset, length);
				Samurai::IO::Buffer& leaves = entry->getLeafdata();
				EXO_ASSERT(onDisk == leaves.copyRange(0, leaves.size()));
			}
		}
	
	return 1;
});

/* Every cache file ends with the record that says so. */
EXO_TEST(hashcache_the_cache_file_ends_with_an_end_of_file_record,
{
		Fixture f;
		const std::string path = f.file("four.bin", std::string(100000, 'd'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			EXO_ASSERT(drain(mgr));
		}

		const std::string raw = readFile(f.cache);
		EXO_ASSERT(raw.size() > sizeof(CacheRecordHeader));

		CacheRecordHeader tail;
		memset(&tail, 0, sizeof(tail));
		if (raw.size() >= sizeof(tail))
			memcpy(&tail, &raw[raw.size() - sizeof(tail)], sizeof(tail));

		EXO_ASSERT(tail.magic == STORAGE_MAGIC_EOF);
		EXO_ASSERT(tail.size == 0);
	
	return 1;
});

/* Several files all come back. */
EXO_TEST(hashcache_several_files_round_trip_through_one_cache,
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
			EXO_ASSERT(drain(mgr));
			EXO_ASSERT(listener.hashed == 3);
		}

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(a), entry));
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(b), entry));
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(c), entry));
	
	return 1;
});

/* A file that has been replaced since is not what we hashed. */
EXO_TEST(hashcache_a_file_whose_size_changed_does_not_keep_its_old_hash,
{
		Fixture f;
		const std::string path = f.file("changed.bin", std::string(100000, 'e'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			EXO_ASSERT(drain(mgr));
		}

		/* Same name, different contents and length. */
		writeFile(path, std::string(120000, 'f'));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File(path), entry));
	
	return 1;
});

/* Nor one that was touched without changing length. */
EXO_TEST(hashcache_a_file_whose_timestamp_changed_does_not_keep_its_hash,
{
		Fixture f;
		const std::string path = f.file("touched.bin", std::string(100000, 'g'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			EXO_ASSERT(drain(mgr));
		}

		struct utimbuf times;
		times.actime = time(0) + 3600;
		times.modtime = time(0) + 3600;
		EXO_ASSERT(utime(path.c_str(), &times) == 0);

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File(path), entry));
	
	return 1;
});

/* Nor one that is gone. */
EXO_TEST(hashcache_a_file_that_is_gone_does_not_stay_in_the_cache,
{
		Fixture f;
		const std::string path = f.file("gone.bin", std::string(100000, 'h'));

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(path), &listener);
			EXO_ASSERT(drain(mgr));
		}

		Samurai::IO::File(path).remove();

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File(path), entry));
	
	return 1;
});

/* A hand made cache is read the same way as one we wrote. */
EXO_TEST(hashcache_a_hand_made_cache_file_reads_back,
{
		Fixture f;
		const std::string path = f.file("hand.bin", "the contents do not matter here");

		Samurai::IO::File probe(path);
		Handmade made;
		made.master();
		made.entry(path, probe.size(), probe.getTimeModified().getInternalData(),
			std::string(24, '\x41'));
		made.eof();
		EXO_ASSERT(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(path), entry));
		EXO_ASSERT(entry && entry->getLeafdataSize() == 24);
		EXO_ASSERT(entry && entry->getSize() == probe.size());
	
	return 1;
});

/* An empty file is not a cache. */
EXO_TEST(hashcache_an_empty_cache_file_is_refused,
{
		Fixture f;
		EXO_ASSERT(writeFile(f.cache, ""));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File("/nonexistent"), entry));
	
	return 1;
});

/* Neither is something else entirely. */
EXO_TEST(hashcache_a_cache_file_that_is_not_one_is_refused,
{
		Fixture f;
		EXO_ASSERT(writeFile(f.cache, std::string(4096, '?')));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File("/nonexistent"), entry));
	
	return 1;
});

/* A cache written by a version that is not this one. */
EXO_TEST(hashcache_a_cache_from_another_version_is_refused_whole,
{
		Fixture f;
		const std::string path = f.file("version.bin", "x");

		Samurai::IO::File probe(path);
		Handmade made;
		made.master(sizeof(size_t), 99);
		made.entry(path, probe.size(), probe.getTimeModified().getInternalData(),
			std::string(24, '\x42'));
		made.eof();
		EXO_ASSERT(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File(path), entry));
	
	return 1;
});

/* And one from a machine whose size_t is a different width. */
EXO_TEST(hashcache_a_cache_from_a_machine_of_another_width_is_refused,
{
		Fixture f;
		const std::string path = f.file("width.bin", "x");

		Samurai::IO::File probe(path);
		Handmade made;
		made.master(sizeof(size_t) == 8 ? 4 : 8, 1);
		made.entry(path, probe.size(), probe.getTimeModified().getInternalData(),
			std::string(24, '\x43'));
		made.eof();
		EXO_ASSERT(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File(path), entry));
	
	return 1;
});

/*
	 * A save that was interrupted. Everything written before the cut has to
	 * survive: it is the difference between rehashing one file and rehashing
	 * the whole share.
	 */
EXO_TEST(hashcache_a_half_written_cache_keeps_the_entries_that_made_it,
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
		EXO_ASSERT(made.saveTruncated(f.cache, afterFirst + 20));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(a), entry));
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File(b), entry));
	
	return 1;
});

/* Cut between two whole entries, with no end of file record. */
EXO_TEST(hashcache_a_cache_with_no_end_of_file_record_still_reads,
{
		Fixture f;
		const std::string a = f.file("nofin.bin", "only file");

		Samurai::IO::File pa(a);
		Handmade made;
		made.master();
		made.entry(a, pa.size(), pa.getTimeModified().getInternalData(), std::string(24, '\x46'));
		EXO_ASSERT(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(a), entry));
	
	return 1;
});

/*
	 * A record claiming more leaf data than the whole file holds. The claim is
	 * a uint32, and the read buffered all of it before finding out the file was
	 * nowhere near that long - four gigabytes of it, for a cache file whose
	 * last sector went bad.
	 */
EXO_TEST(hashcache_a_record_claiming_more_than_the_file_holds_is_refused,
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
		EXO_ASSERT(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(!mgr.lookup(Samurai::IO::File(a), entry));
	
	return 1;
});

/* A record whose magic is not one we know stops the read there. */
EXO_TEST(hashcache_an_unknown_record_stops_the_read_rather_than_the_process,
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
		EXO_ASSERT(made.save(f.cache));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		QuickDC::Hash::CacheEntry* entry = 0;
		EXO_ASSERT(mgr.lookup(Samurai::IO::File(a), entry));
	
	return 1;
});

/*
	 * Taking a job back. ~Share::File does this for a file that is dropped
	 * before its hash arrived - a share rebuilt while hashing is still going -
	 * so from here on nothing may be reported about it: the listener is the
	 * Share::File, and it is on its way out.
	 *
	 * Which of the three states the job is in is a matter of timing. It may
	 * still be queued, it may be the one the worker is on, or it may have
	 * finished and be waiting to be collected. The listener hears nothing in
	 * any of them, which is what makes this assertion timing independent.
	 */
EXO_TEST(hashcache_a_job_that_was_taken_back_is_not_reported_to_its,
{
		Fixture f;
		const std::string busy = f.file("busy.bin", std::string(6 * 1024 * 1024, 'j'));
		const std::string gone = f.file("dropped.bin", std::string(200000, 'k'));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		Listener keep, drop;

		mgr.addJob(Samurai::IO::File(busy), &keep);
		mgr.addJob(Samurai::IO::File(gone), &drop);
		EXO_ASSERT(drop.queued == 1);

		mgr.removeJob(Samurai::IO::File(gone));

		EXO_ASSERT(drain(mgr));
		EXO_ASSERT(keep.hashed == 1);
		EXO_ASSERT(drop.hashed == 0);
		EXO_ASSERT(drop.progress == 0);
	
	return 1;
});

/*
	 * And the shape that matters: the listener is freed as soon as it has taken
	 * its job back, as a Share::File is. A job that kept a pointer to it would
	 * be calling into freed memory the next time the main loop came round.
	 */
EXO_TEST(hashcache_a_job_whose_listener_has_gone_is_drained_without_calling_it,
{
		Fixture f;
		const std::string path = f.file("orphan.bin", std::string(4 * 1024 * 1024, 'l'));

		QuickDC::Hash::Manager mgr(f.cache.c_str());
		Listener* leaving = new Listener();

		mgr.addJob(Samurai::IO::File(path), leaving);
		mgr.removeJob(Samurai::IO::File(path));
		delete leaving;

		EXO_ASSERT(drain(mgr));
	
	return 1;
});

/* Taking back a job the manager does not have is not an error. */
EXO_TEST(hashcache_removing_a_job_that_was_never_added_does_nothing,
{
		Fixture f;
		QuickDC::Hash::Manager mgr(f.cache.c_str());
		mgr.removeJob(Samurai::IO::File("/nonexistent"));
		EXO_ASSERT(!mgr.isBusy());
	
	return 1;
});

/* Rewriting the cache replaces it rather than growing it. */
EXO_TEST(hashcache_saving_the_cache_again_replaces_it,
{
		Fixture f;
		const std::string a = f.file("rewrite.bin", std::string(80000, 'i'));

		size_t first = 0;
		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(a), &listener);
			EXO_ASSERT(drain(mgr));
		}
		first = readFile(f.cache).size();
		EXO_ASSERT(first > 0);

		{
			QuickDC::Hash::Manager mgr(f.cache.c_str());
			Listener listener;
			mgr.addJob(Samurai::IO::File(a), &listener);
			EXO_ASSERT(drain(mgr));
		}
		EXO_ASSERT(readFile(f.cache).size() == first);
	
	return 1;
});
