/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * What happens to a download once the last byte of it has arrived: the file is
 * hashed, compared against the TTH it was queued with, moved from
 * getTempFilename() to getLocalFilename(), announced, and taken off the queue.
 *
 * Every step of that runs somewhere else - commitChunk() only notices that the
 * chunk map is full, the hashing happens on the hash manager's worker thread,
 * and the result is applied by the next process() - so the whole path is driven
 * here rather than any single call being asserted.
 *
 * A download's local filename comes from [Downloads]/Directory, which defaults
 * to "~/.quickdc/downloads": the name the queue holds is whatever was
 * configured, while everything that opens it goes through Samurai::IO::File and
 * therefore sees a resolved path. The cases below queue their files by a '~'
 * name for that reason, with HOME pointed at a scratch directory.
 */

#include "quickdc.h"
#include "api/core.h"
#include "hash/hashjob.h"
#include "hash/hashmanager.h"
#include "network/downloadqueue.h"

#include <samurai/io/file.h>

#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>

/* A scratch home, so "~/downloads/..." is somewhere harmless. It carries the
   process id, because under DISCOVER each case is a process of its own. */
static const std::string TEST_HOME =
	"/tmp/quickdc_complete_home_" + std::to_string((int) getpid());

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Deterministic filler, so a failure is reproducible. */
static std::string makeBytes(size_t length)
{
	std::string out;
	out.reserve(length);

	uint32_t state = 0x12345678u;
	for (size_t n = 0; n < length; n++)
	{
		state = state * 1103515245u + 12345u;
		out += (char) ((state >> 16) & 0xff);
	}
	return out;
}

static bool writeFile(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(Samurai::IO::File::resolvePath(path).c_str(), "wb");
	if (!fh) return false;

	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

static bool exists(const std::string& path)
{
	return Samurai::IO::File(path).exists();
}

static void removeFile(const std::string& path)
{
	Samurai::IO::File(path).remove();
}

/* Reads a file back, to prove the finished download is the bytes we wrote. */
static std::string readFile(const std::string& path)
{
	std::string out;

	FILE* fh = fopen(Samurai::IO::File::resolvePath(path).c_str(), "rb");
	if (!fh) return out;

	char buf[4096];
	size_t got = 0;
	while ((got = fread(buf, 1, sizeof(buf), fh)) > 0)
		out.append(buf, got);

	fclose(fh);
	return out;
}

/* Everything the queue announces, kept because a QueueFile does not outlive
   the call it is handed over in. */
class Watcher : public QuickDC::QueueListener
{
	public:
		void EventQueueFileAdded(const QuickDC::QueueFile*) override { added++; }
		void EventQueueFileRemoved(const QuickDC::QueueFile*) override { removed++; }

		void EventQueueFileVerified(const QuickDC::QueueFile* file) override
		{
			verified++;
			if (file) name = file->getLocalFilename();
		}

		size_t added = 0;
		size_t removed = 0;
		size_t verified = 0;
		std::string name;
};

/* Asks the hash manager what a file's TTH is, rather than writing one down. */
class Hasher : public QuickDC::Hash::JobListener
{
	public:
		void EventFileStarted(const QuickDC::Hash::Job*) override { }
		void EventFileQueued(const QuickDC::Hash::Job*) override { }
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) override { }

		void EventFileHashed(const QuickDC::Hash::Job* job) override
		{
			/* getHashSum() hands back a static buffer, so copy it. */
			if (job) tth = job->getHashSum();
			done = true;
		}

		std::string tth;
		bool done = false;
};

/*
 * The main loop, as far as a finished download is concerned: collect what the
 * hash worker has produced, then let the queue apply it.
 *
 * Returns whether 'ready' came true within the timeout, so a case can assert on
 * the outcome rather than on having waited long enough.
 */
template<typename Predicate>
static bool pump(QuickDC::QueueManager* queue, Predicate ready, int timeout_ms = 10000)
{
	QuickDC::Core* core = QuickDC::Core::getInstance();

	for (int waited = 0; waited <= timeout_ms; waited += 10)
	{
		if (core && core->hash) core->hash->process();
		if (queue) queue->process();

		if (ready()) return true;
		usleep(10000);
	}
	return ready();
}

static std::string tthOf(const std::string& path)
{
	Hasher hasher;
	QuickDC::Core::getInstance()->hash->addJob(Samurai::IO::File(path), &hasher);

	pump(0, [&] { return hasher.done; });
	return hasher.tth;
}

/* ------------------------------------------------------------------------- */
/* One queued file whose bytes are all on disk, waiting to be finished        */
/* ------------------------------------------------------------------------- */

class Download
{
	public:
		Download(const char* leaf, size_t size, bool withTTH = true)
			: bytes(makeBytes(size))
			, name(std::string("~/downloads/") + leaf)
			, queue_path(TEST_HOME + "/queue_" + leaf + ".xml")
		{
			/* Hashed under a name of its own, so the download's own name is not
			   in the hash cache and the real hashing path is exercised. */
			source_path = TEST_HOME + "/source_" + leaf;
			if (!writeFile(source_path, bytes)) ok = false;
			tth = withTTH ? tthOf(source_path) : std::string();

			queue = std::make_unique<QuickDC::QueueManager>(queue_path.c_str());
			queue->addListener(&watcher);

			file = queue->add(name.c_str(), withTTH ? tth.c_str() : 0, size);
			if (!file) { ok = false; return; }

			/* As a transfer would have left it: every byte written, but the
			   chunk map not yet told about any of it. */
			if (!writeFile(file->getTempFilename(), bytes)) ok = false;
		}

		~Download()
		{
			if (queue)
			{
				queue->removeListener(&watcher);
				queue.reset();
			}

			removeFile(source_path);
			removeFile(name);
			removeFile(name + QUICKDC_PART_SUFFIX);
			removeFile(queue_path);
			removeFile(queue_path + ".new");
		}

		/* Waits for the entry to leave the queue, which is the last of the four
		   things a finished download does. */
		bool settle() { return pump(queue.get(), [&] { return queue->size() == 0; }); }

		std::string bytes;
		std::string name;
		std::string tth;
		std::string source_path;
		std::string queue_path;
		Watcher watcher;
		std::unique_ptr<QuickDC::QueueManager> queue;
		QuickDC::QueueFile* file = 0;

		/* Whether the scratch files were written and the entry queued: a
		   constructor cannot assert, so a case does it on its behalf. */
		bool ok = true;
};

/* ------------------------------------------------------------------------- */

namespace {

/*
 * The fixture: the scratch home, and a core pointed into it.
 */
std::string config_file;
std::string session_file;
std::string hashdb_file;
QuickDC::Core* core = 0;

}

EXO_SETUP({
	setenv("HOME", TEST_HOME.c_str(), 1);

	Samurai::IO::File::mkdir(TEST_HOME.c_str(), 0700);
	Samurai::IO::File::mkdir((TEST_HOME + "/downloads").c_str(), 0700);
	Samurai::IO::File::mkdir((TEST_HOME + "/.quickdc").c_str(), 0700);

	config_file = TEST_HOME + "/quickdc.conf";
	session_file = TEST_HOME + "/session.conf";
	hashdb_file = TEST_HOME + "/hashdb.cache";

	/* A core with no queue of its own: each case brings one, pointed at a
	   file of its own. The hash manager is what the completion path reaches
	   for through the core, and this constructor does not build one. */
	core = new QuickDC::Core(config_file, session_file, std::string());
	core->hash = new QuickDC::Hash::Manager(hashdb_file.c_str());
	return 1;
});

EXO_TEARDOWN({
	/* Before the files, which it holds open and rewrites as it goes. */
	delete core;
	core = 0;

	removeFile(config_file);
	removeFile(session_file);
	removeFile(hashdb_file);
	removeFile(hashdb_file + ".new");

	/* The share manager writes these when the last hash job is collected. */
	removeFile(TEST_HOME + "/.quickdc/files.xml");
	removeFile(TEST_HOME + "/.quickdc/files.xml.bz2");

	Samurai::IO::File::rmdir((TEST_HOME + "/.quickdc").c_str());
	Samurai::IO::File::rmdir((TEST_HOME + "/downloads").c_str());
	Samurai::IO::File::rmdir(TEST_HOME.c_str());
	return 1;
});

/* The whole file delivered by one peer in one go. */
EXO_TEST(queuecomplete_a_file_committed_in_one_chunk_is_verified_renamed_and_dequeued,
{
	Download dl("one.bin", 5763);
	EXO_ASSERT(dl.ok);

	EXO_ASSERT(!dl.tth.empty());
	EXO_ASSERT(dl.file->getDoneBytes() == 0);
	EXO_ASSERT(!dl.file->isComplete());

	dl.queue->commitChunk(dl.file, 0, dl.bytes.size());
	EXO_ASSERT(dl.settle());

	EXO_ASSERT(!exists(dl.name + QUICKDC_PART_SUFFIX));
	EXO_ASSERT(exists(dl.name));
	EXO_ASSERT(readFile(dl.name) == dl.bytes);
	EXO_ASSERT(dl.queue->size() == 0);
	EXO_ASSERT(dl.watcher.verified == 1);
	EXO_ASSERT(dl.watcher.removed == 1);
	EXO_ASSERT(dl.watcher.name == dl.name);
	return 1;
});

/* The same file, committed a chunk at a time as several peers deliver. */
EXO_TEST(queuecomplete_a_file_committed_in_several_chunks_is_verified_renamed_and_dequeued,
{
	Download dl("many.bin", 300000);
	EXO_ASSERT(dl.ok);

	const uint64_t size = dl.bytes.size();
	const uint64_t third = size / 3;

	/* Deliberately out of order, and not on a chunk boundary. */
	dl.queue->commitChunk(dl.file, third, third);
	EXO_ASSERT(!dl.file->isComplete());
	EXO_ASSERT(dl.queue->size() == 1);

	dl.queue->commitChunk(dl.file, 2 * third, size - 2 * third);
	EXO_ASSERT(!dl.file->isComplete());
	EXO_ASSERT(dl.queue->size() == 1);

	dl.queue->commitChunk(dl.file, 0, third);
	EXO_ASSERT(dl.settle());

	EXO_ASSERT(!exists(dl.name + QUICKDC_PART_SUFFIX));
	EXO_ASSERT(exists(dl.name));
	EXO_ASSERT(readFile(dl.name) == dl.bytes);
	EXO_ASSERT(dl.queue->size() == 0);
	EXO_ASSERT(dl.watcher.verified == 1);
	EXO_ASSERT(dl.watcher.removed == 1);
	return 1;
});

/*
 * A file the hash manager has already seen. addJob() answers from its cache
 * by calling the listener back from inside commitChunk(), so this is the
 * path where the result arrives before the queue has returned.
 */
EXO_TEST(queuecomplete_a_file_already_in_the_hash_cache_is_finished_just_the_same,
{
	Download dl("cached.bin", 4096);
	EXO_ASSERT(dl.ok);

	EXO_ASSERT(tthOf(dl.file->getTempFilename()) == dl.tth);

	dl.queue->commitChunk(dl.file, 0, dl.bytes.size());
	EXO_ASSERT(dl.settle());

	EXO_ASSERT(!exists(dl.name + QUICKDC_PART_SUFFIX));
	EXO_ASSERT(exists(dl.name));
	EXO_ASSERT(dl.queue->size() == 0);
	EXO_ASSERT(dl.watcher.verified == 1);
	return 1;
});

/* Nothing to check it against, so it is taken at face value. */
EXO_TEST(queuecomplete_a_file_with_no_tth_is_renamed_and_dequeued_without_hashing,
{
	Download dl("notth.bin", 2048, false);
	EXO_ASSERT(dl.ok);

	EXO_ASSERT(dl.file->getTTH() == 0);

	dl.queue->commitChunk(dl.file, 0, dl.bytes.size());

	/* No hashing involved: it is done by the time commitChunk() returns. */
	EXO_ASSERT(!exists(dl.name + QUICKDC_PART_SUFFIX));
	EXO_ASSERT(exists(dl.name));
	EXO_ASSERT(readFile(dl.name) == dl.bytes);
	EXO_ASSERT(dl.queue->size() == 0);
	EXO_ASSERT(dl.watcher.removed == 1);
	return 1;
});

/*
 * Somebody fed us bad data. Without leaf hashes there is no telling which
 * chunk was at fault, so the file has to be fetched again from nothing - and
 * it stays queued, which is the one case where that is right.
 */
EXO_TEST(queuecomplete_a_file_that_fails_its_hash_is_started_over_and_stays_queued,
{
	Download dl("bad.bin", 6000);
	EXO_ASSERT(dl.ok);

	/* Queued as one file, but a different one turned up. */
	EXO_ASSERT(writeFile(dl.file->getTempFilename(), makeBytes(6000) + "x"));

	dl.queue->commitChunk(dl.file, 0, dl.bytes.size());
	EXO_ASSERT(pump(dl.queue.get(), [&] { return dl.file->getDoneBytes() == 0; }));

	EXO_ASSERT(!exists(dl.name));
	EXO_ASSERT(!exists(dl.name + QUICKDC_PART_SUFFIX));
	EXO_ASSERT(dl.queue->size() == 1);
	EXO_ASSERT(dl.watcher.verified == 0);
	EXO_ASSERT(dl.watcher.removed == 0);
	EXO_ASSERT(!dl.file->isComplete());
	return 1;
});
