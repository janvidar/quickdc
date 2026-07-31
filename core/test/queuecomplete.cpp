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

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* A scratch home, so "~/downloads/..." is somewhere harmless. */
#define TEST_HOME "/tmp/quickdc_complete_home"

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
		void EventQueueFileAdded(const QuickDC::QueueFile*) { added++; }
		void EventQueueFileRemoved(const QuickDC::QueueFile*) { removed++; }

		void EventQueueFileVerified(const QuickDC::QueueFile* file)
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
		void EventFileStarted(const QuickDC::Hash::Job*) { }
		void EventFileQueued(const QuickDC::Hash::Job*) { }
		void EventFileHashProgress(const QuickDC::Hash::Job*, uint64_t, uint64_t) { }

		void EventFileHashed(const QuickDC::Hash::Job* job)
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
			, queue_path(std::string(TEST_HOME) + "/queue_" + leaf + ".xml")
		{
			/* Hashed under a name of its own, so the download's own name is not
			   in the hash cache and the real hashing path is exercised. */
			source_path = std::string(TEST_HOME) + "/source_" + leaf;
			CHECK(writeFile(source_path, bytes));
			tth = withTTH ? tthOf(source_path) : std::string();

			queue = std::make_unique<QuickDC::QueueManager>(queue_path.c_str());
			queue->addListener(&watcher);

			file = queue->add(name.c_str(), withTTH ? tth.c_str() : 0, size);

			/* As a transfer would have left it: every byte written, but the
			   chunk map not yet told about any of it. */
			CHECK(writeFile(file->getTempFilename(), bytes));
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
};

/* ------------------------------------------------------------------------- */

static void test_complete()
{
	/* The whole file delivered by one peer in one go. */
	{
		Download dl("one.bin", 5763);

		CHECK(!dl.tth.empty());
		CHECK(dl.file->getDoneBytes() == 0);
		CHECK(!dl.file->isComplete());

		dl.queue->commitChunk(dl.file, 0, dl.bytes.size());
		CHECK(dl.settle());

		CHECK(!exists(dl.name + QUICKDC_PART_SUFFIX));
		CHECK(exists(dl.name));
		CHECK(readFile(dl.name) == dl.bytes);
		CHECK(dl.queue->size() == 0);
		CHECK(dl.watcher.verified == 1);
		CHECK(dl.watcher.removed == 1);
		CHECK(dl.watcher.name == dl.name);
		printf("  ok a file committed in one chunk is verified, renamed and dequeued\n");
	}

	/* The same file, committed a chunk at a time as several peers deliver. */
	{
		Download dl("many.bin", 300000);

		const uint64_t size = dl.bytes.size();
		const uint64_t third = size / 3;

		/* Deliberately out of order, and not on a chunk boundary. */
		dl.queue->commitChunk(dl.file, third, third);
		CHECK(!dl.file->isComplete());
		CHECK(dl.queue->size() == 1);

		dl.queue->commitChunk(dl.file, 2 * third, size - 2 * third);
		CHECK(!dl.file->isComplete());
		CHECK(dl.queue->size() == 1);

		dl.queue->commitChunk(dl.file, 0, third);
		CHECK(dl.settle());

		CHECK(!exists(dl.name + QUICKDC_PART_SUFFIX));
		CHECK(exists(dl.name));
		CHECK(readFile(dl.name) == dl.bytes);
		CHECK(dl.queue->size() == 0);
		CHECK(dl.watcher.verified == 1);
		CHECK(dl.watcher.removed == 1);
		printf("  ok a file committed in several chunks is verified, renamed and dequeued\n");
	}

	/*
	 * A file the hash manager has already seen. addJob() answers from its cache
	 * by calling the listener back from inside commitChunk(), so this is the
	 * path where the result arrives before the queue has returned.
	 */
	{
		Download dl("cached.bin", 4096);

		CHECK(tthOf(dl.file->getTempFilename()) == dl.tth);

		dl.queue->commitChunk(dl.file, 0, dl.bytes.size());
		CHECK(dl.settle());

		CHECK(!exists(dl.name + QUICKDC_PART_SUFFIX));
		CHECK(exists(dl.name));
		CHECK(dl.queue->size() == 0);
		CHECK(dl.watcher.verified == 1);
		printf("  ok a file already in the hash cache is finished just the same\n");
	}

	/* Nothing to check it against, so it is taken at face value. */
	{
		Download dl("notth.bin", 2048, false);

		CHECK(dl.file->getTTH() == 0);

		dl.queue->commitChunk(dl.file, 0, dl.bytes.size());

		/* No hashing involved: it is done by the time commitChunk() returns. */
		CHECK(!exists(dl.name + QUICKDC_PART_SUFFIX));
		CHECK(exists(dl.name));
		CHECK(readFile(dl.name) == dl.bytes);
		CHECK(dl.queue->size() == 0);
		CHECK(dl.watcher.removed == 1);
		printf("  ok a file with no tth is renamed and dequeued without hashing\n");
	}

	/*
	 * Somebody fed us bad data. Without leaf hashes there is no telling which
	 * chunk was at fault, so the file has to be fetched again from nothing - and
	 * it stays queued, which is the one case where that is right.
	 */
	{
		Download dl("bad.bin", 6000);

		/* Queued as one file, but a different one turned up. */
		CHECK(writeFile(dl.file->getTempFilename(), makeBytes(6000) + "x"));

		dl.queue->commitChunk(dl.file, 0, dl.bytes.size());
		CHECK(pump(dl.queue.get(), [&] { return dl.file->getDoneBytes() == 0; }));

		CHECK(!exists(dl.name));
		CHECK(!exists(dl.name + QUICKDC_PART_SUFFIX));
		CHECK(dl.queue->size() == 1);
		CHECK(dl.watcher.verified == 0);
		CHECK(dl.watcher.removed == 0);
		CHECK(!dl.file->isComplete());
		printf("  ok a file that fails its hash is started over and stays queued\n");
	}
}

int main()
{
	setbuf(stdout, 0);

	setenv("HOME", TEST_HOME, 1);

	Samurai::IO::File::mkdir(TEST_HOME, 0700);
	Samurai::IO::File::mkdir(TEST_HOME "/downloads", 0700);
	Samurai::IO::File::mkdir(TEST_HOME "/.quickdc", 0700);

	const std::string config = TEST_HOME "/quickdc.conf";
	const std::string session = TEST_HOME "/session.conf";
	const std::string hashdb = TEST_HOME "/hashdb.cache";

	{
		/* A core with no queue of its own: each case brings one, pointed at a
		   file of its own. The hash manager is what the completion path reaches
		   for through the core, and this constructor does not build one. */
		QuickDC::Core core(config, session, std::string());
		core.hash = new QuickDC::Hash::Manager(hashdb.c_str());

		test_complete();
	}

	removeFile(config);
	removeFile(session);
	removeFile(hashdb);
	removeFile(hashdb + ".new");

	/* The share manager writes these when the last hash job is collected. */
	removeFile(TEST_HOME "/.quickdc/files.xml");
	removeFile(TEST_HOME "/.quickdc/files.xml.bz2");

	Samurai::IO::File::rmdir(TEST_HOME "/.quickdc");
	Samurai::IO::File::rmdir(TEST_HOME "/downloads");
	Samurai::IO::File::rmdir(TEST_HOME);

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
