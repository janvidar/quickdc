#include <samurai/samurai.h>
#include <string.h>
#include <stdlib.h>

#include <samurai/io/file.h>

#include "network/chunkmap.h"
#include "network/filesource.h"
#include "network/downloadqueue.h"
#include "network/dc/commands.h"
#include "network/dc/hubcommands.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "network/securitymanager.h"

#define QUEUE_TEST_TTH_A "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define QUEUE_TEST_TTH_B "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

/* A CID is the same shape as a TTH root: 39 base32 characters. */
#define QUEUE_TEST_CID_A "ACSTOWQXZASAQ7EYRJ4KLMNPQ2345XYZABCDEFG"
#define QUEUE_TEST_CID_B "Q2AH7321WAS1PQRSTUVWXYZ234567ABCDEFGHIJ"

/*
 * These tables live out here on purpose: EXO_TEST() takes the test body as a
 * single macro argument, so a brace-enclosed initializer list inside one would
 * be split on its commas and read as extra arguments.
 */

/** Chunk notations that must all be rejected. */
static const char* queue_test_bad_chunks[] = {
	"0-100,",        /* trailing separator */
	"abc",           /* not numbers at all */
	"100-100",       /* empty range */
	"200-100",       /* backwards */
	"0-100;200-300", /* wrong separator */
	"0-",            /* missing end */
	"-100",          /* missing start */
	0
};

/** Every source URL form documented in filesource.h. */
static const char* queue_test_urls[] = {
	"adc://adchub:7817/ACSTOWQXZASA/TTH:" QUEUE_TEST_TTH_A,
	"dchub://myhub:411/NBSWY3DP/file.bin",
	"adc://host:7817/CID/dir/",   /* a directory */
	"adc://host:7817/CID/",       /* the root directory */
	"adc://host:7817/CID",        /* the file list */
	"adcs://host:7817/CID/deep/path/f.iso",
	0
};

/** Source URLs that carry no usable hub and client ID. */
static const char* queue_test_bad_urls[] = {
	"",
	"notaurl",
	"adc://host:7817",     /* no client ID */
	"adc://host:7817/",    /* empty client ID */
	"://host/CID",         /* no protocol */
	"adc:/host/CID",       /* not a URL */
	0
};

/* ------------------------------------------------------------------------- *
 * ChunkMap
 * ------------------------------------------------------------------------- */

EXO_TEST(chunkmap_coalesce_adjacent, {
	QuickDC::ChunkMap m(2048);
	m.addRange(0, 1024);
	m.addRange(1024, 2048);
	/* Two touching ranges have to become one, or the file never reads as done. */
	return m.getRangeCount() == 1 && m.isComplete() && m.getDoneBytes() == 2048;
});

EXO_TEST(chunkmap_coalesce_overlapping, {
	QuickDC::ChunkMap m(1000);
	m.addRange(500, 700);
	m.addRange(100, 200);
	m.addRange(150, 600);

	uint64_t start = 0;
	uint64_t end = 0;
	return m.getRangeCount() == 1
		&& m.getRange(0, start, end)
		&& start == 100 && end == 700
		&& m.getDoneBytes() == 600
		&& !m.isComplete();
});

EXO_TEST(chunkmap_double_add_does_not_inflate, {
	QuickDC::ChunkMap m(1000);
	m.addRange(0, 500);
	m.addRange(0, 500);
	m.addRange(100, 200);
	return m.getDoneBytes() == 500 && m.getRangeCount() == 1;
});

EXO_TEST(chunkmap_clamps_to_size, {
	QuickDC::ChunkMap m(100);
	m.addRange(50, 500);
	return m.getDoneBytes() == 50 && !m.isComplete();
});

EXO_TEST(chunkmap_unknown_size_is_never_complete, {
	QuickDC::ChunkMap m;
	uint64_t start = 0;
	uint64_t length = 0;
	return !m.isComplete() && !m.reserve(1024, start, length);
});

EXO_TEST(chunkmap_tostring, {
	QuickDC::ChunkMap m(2097152);
	m.addRange(0, 262144);
	m.addRange(524288, 1048576);
	/* The notation documented in downloadqueue.h. */
	return m.toString() == "0-262144,524288-1048576";
});

EXO_TEST(chunkmap_fromstring_roundtrip, {
	QuickDC::ChunkMap m(2097152);
	if (!m.fromString("0-262144,524288-1048576")) return false;
	if (m.getRangeCount() != 2) return false;
	if (m.getDoneBytes() != 262144 + 524288) return false;
	return m.toString() == "0-262144,524288-1048576";
});

EXO_TEST(chunkmap_fromstring_empty, {
	QuickDC::ChunkMap m(1000);
	return m.fromString("") && m.getRangeCount() == 0;
});

EXO_TEST(chunkmap_fromstring_rejects_malformed, {
	QuickDC::ChunkMap m(1000);

	/* Each of these must be refused, and must leave the map empty rather
	   than half parsed. */
	for (size_t n = 0; queue_test_bad_chunks[n]; n++)
	{
		if (m.fromString(queue_test_bad_chunks[n])) return false;
		if (m.getRangeCount() != 0) return false;
	}
	return true;
});

EXO_TEST(chunkmap_reserve_skips_completed, {
	QuickDC::ChunkMap m(1000);
	m.addRange(0, 400);

	uint64_t start = 0;
	uint64_t length = 0;
	return m.reserve(100, start, length) && start == 400 && length == 100;
});

EXO_TEST(chunkmap_reserve_skips_other_reservations, {
	QuickDC::ChunkMap m(1000);

	uint64_t s1 = 0;
	uint64_t l1 = 0;
	uint64_t s2 = 0;
	uint64_t l2 = 0;
	if (!m.reserve(100, s1, l1)) return false;
	if (!m.reserve(100, s2, l2)) return false;

	return s1 == 0 && s2 == 100 && m.getReservationCount() == 2;
});

EXO_TEST(chunkmap_reserve_clamped_by_next_range, {
	QuickDC::ChunkMap m(1000);
	m.addRange(0, 400);
	m.addRange(700, 1000);

	uint64_t start = 0;
	uint64_t length = 0;
	/* Asked for far more than the gap holds. */
	return m.reserve(4096, start, length) && start == 400 && length == 300;
});

EXO_TEST(chunkmap_commit_promotes, {
	QuickDC::ChunkMap m(1000);
	m.addRange(0, 400);

	uint64_t start = 0;
	uint64_t length = 0;
	if (!m.reserve(600, start, length)) return false;

	m.commit(start, length);
	return m.isComplete() && m.getReservationCount() == 0;
});

EXO_TEST(chunkmap_commit_shorter_than_reserved, {
	QuickDC::ChunkMap m(1000);

	uint64_t start = 0;
	uint64_t length = 0;
	if (!m.reserve(1000, start, length)) return false;
	if (length != 1000) return false;

	/* A peer is allowed to send less than it was asked for. The remainder
	   has to become available again, not stay reserved forever. */
	m.commit(start, 400);
	if (m.getReservationCount() != 0) return false;
	if (m.getDoneBytes() != 400) return false;

	uint64_t next = 0;
	uint64_t nextLength = 0;
	return m.reserve(600, next, nextLength) && next == 400 && nextLength == 600;
});

EXO_TEST(chunkmap_release_hands_back, {
	QuickDC::ChunkMap m(1000);

	uint64_t start = 0;
	uint64_t length = 0;
	if (!m.reserve(500, start, length)) return false;

	m.release(start, length);
	if (m.getReservationCount() != 0) return false;
	if (m.getDoneBytes() != 0) return false;

	uint64_t again = 0;
	uint64_t againLength = 0;
	return m.reserve(500, again, againLength) && again == 0 && againLength == 500;
});

EXO_TEST(chunkmap_nothing_left_to_reserve, {
	QuickDC::ChunkMap m(100);
	m.addRange(0, 100);

	uint64_t start = 0;
	uint64_t length = 0;
	return !m.reserve(10, start, length);
});

EXO_TEST(chunkmap_clear_forgets_progress, {
	QuickDC::ChunkMap m(1000);
	m.addRange(0, 1000);
	m.clear();
	return m.getDoneBytes() == 0 && m.getSize() == 1000 && !m.isComplete();
});

EXO_TEST(chunkmap_setsize_truncates, {
	QuickDC::ChunkMap m(1000);
	m.addRange(0, 1000);
	m.setSize(500);
	return m.getDoneBytes() == 500 && m.isComplete();
});

/* ------------------------------------------------------------------------- *
 * FileSource
 * ------------------------------------------------------------------------- */

EXO_TEST(filesource_url_roundtrip, {
	/* Every form documented in filesource.h has to survive decode/encode. */
	for (size_t n = 0; queue_test_urls[n]; n++)
	{
		QuickDC::FileSource src(queue_test_urls[n]);
		if (!src.isValid()) return false;

		char* out = src.toURL();
		if (!out) return false;

		bool ok = (strcmp(out, queue_test_urls[n]) == 0);
		free(out);
		if (!ok) return false;
	}
	return true;
});

EXO_TEST(filesource_splits_parts, {
	QuickDC::FileSource src("adc://adchub:7817/ACSTOWQXZASA/TTH:ABC");
	return src.isValid()
		&& !strcmp(src.getHubUrl(), "adc://adchub:7817")
		&& !strcmp(src.getClientID(), "ACSTOWQXZASA")
		&& !strcmp(src.getName(), "TTH:ABC")
		&& !src.isFilelist();
});

EXO_TEST(filesource_filelist_vs_root_directory, {
	/* No trailing slash means the file list; a trailing slash means the
	   root directory. The difference is a null versus empty filename. */
	QuickDC::FileSource list("adc://host:7817/CID");
	QuickDC::FileSource root("adc://host:7817/CID/");

	return list.isFilelist() && list.getName() == 0
		&& !root.isFilelist() && root.getName() && !strcmp(root.getName(), "");
});

EXO_TEST(filesource_rejects_malformed, {
	for (size_t n = 0; queue_test_bad_urls[n]; n++)
	{
		QuickDC::FileSource src(queue_test_bad_urls[n]);
		if (src.isValid()) return false;
		if (src.toURL() != 0) return false;
	}

	QuickDC::FileSource nil((const char*) 0);
	return !nil.isValid();
});

EXO_TEST(filesource_ctor_agrees_with_url, {
	QuickDC::FileSource src("adc://host:7817", "CID", "file.bin");
	char* out = src.toURL();
	bool ok = (out && !strcmp(out, "adc://host:7817/CID/file.bin"));
	free(out);
	return ok;
});

EXO_TEST(filesource_matches, {
	QuickDC::FileSource src("adc://host:7817/CID/f");
	return src.matches("adc://host:7817", "CID")
		&& !src.matches("adc://other:7817", "CID")
		&& !src.matches("adc://host:7817", "OTHER")
		&& !src.matches(0, 0);
});

EXO_TEST(filesource_expires_after_retries, {
	QuickDC::FileSource src("adc://host:7817/CID/f");
	src.touch();
	if (src.isExpired()) return false;

	for (int n = 0; n <= QUICKDC_SOURCE_MAX_RETRIES; n++)
		src.onFailure();

	if (!src.isExpired()) return false;

	/* Being seen again clears the slate. */
	src.touch();
	return !src.isExpired() && src.getRetryCount() == 0;
});

EXO_TEST(filesource_expires_when_stale, {
	QuickDC::FileSource src("adc://host:7817/CID/f");
	src.touch();
	if (src.isExpired()) return false;

	time_t old = Samurai::TimeStamp().getInternalData() - (2 * QUICKDC_SOURCE_MAX_AGE);
	src.setLastSeen(Samurai::TimeStamp(old));
	return src.isExpired();
});

/* ------------------------------------------------------------------------- *
 * QueueFile
 * ------------------------------------------------------------------------- */

EXO_TEST(queuefile_priority_names, {
	return !strcmp(QuickDC::QueueFile::convertPriority(QuickDC::QueueFile::PrioPaused), "paused")
		&& !strcmp(QuickDC::QueueFile::convertPriority(QuickDC::QueueFile::PrioLow), "low")
		&& !strcmp(QuickDC::QueueFile::convertPriority(QuickDC::QueueFile::PrioNormal), "normal")
		&& !strcmp(QuickDC::QueueFile::convertPriority(QuickDC::QueueFile::PrioHigh), "high")
		&& QuickDC::QueueFile::convertPriority("paused") == QuickDC::QueueFile::PrioPaused
		&& QuickDC::QueueFile::convertPriority("LOW") == QuickDC::QueueFile::PrioLow
		&& QuickDC::QueueFile::convertPriority("High") == QuickDC::QueueFile::PrioHigh
		&& QuickDC::QueueFile::convertPriority("nonsense") == QuickDC::QueueFile::PrioNormal
		&& QuickDC::QueueFile::convertPriority(0) == QuickDC::QueueFile::PrioNormal;
});

EXO_TEST(queuefile_tempname, {
	QuickDC::QueueFile f("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	return !strcmp(f.getLocalFilename(), "/tmp/dl/file.bin")
		&& !strcmp(f.getTempFilename(), "/tmp/dl/file.bin" QUICKDC_PART_SUFFIX);
});

EXO_TEST(queuefile_persistent_needs_tth_and_size, {
	/* This is the "not every queued download should be stored" rule. */
	QuickDC::QueueFile good("/tmp/dl/a.bin", QUEUE_TEST_TTH_A, 1000);
	QuickDC::QueueFile noTth("/tmp/dl/b.bin", 0, 1000);
	QuickDC::QueueFile shortTth("/tmp/dl/c.bin", "TOOSHORT", 1000);
	QuickDC::QueueFile noSize("/tmp/dl/d.bin", QUEUE_TEST_TTH_A, 0);

	if (!good.isPersistent()) return false;
	if (noTth.isPersistent()) return false;
	if (shortTth.isPersistent()) return false;
	if (noSize.isPersistent()) return false;

	/* A finished file has nothing left to resume. */
	good.getChunks().addRange(0, 1000);
	return good.isComplete() && !good.isPersistent();
});

EXO_TEST(queuefile_ignores_duplicate_sources, {
	QuickDC::QueueFile f("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);

	f.addSource(new QuickDC::FileSource("adc://h:1/CID/f"));
	f.addSource(new QuickDC::FileSource("adc://h:1/CID/f"));
	if (f.countSources() != 1) return false;

	/* An unusable source is dropped rather than stored. */
	f.addSource(new QuickDC::FileSource("garbage"));
	if (f.countSources() != 1) return false;

	f.addSource(new QuickDC::FileSource("adc://h:1/OTHER/f"));
	return f.countSources() == 2;
});

EXO_TEST(queuefile_active_sources, {
	QuickDC::QueueFile f("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	f.addSource(new QuickDC::FileSource("adc://h:1/A/f"));
	f.addSource(new QuickDC::FileSource("adc://h:1/B/f"));

	QuickDC::FileSource* first = f.getIdleSource();
	if (!first) return false;

	f.setSourceActive(first, true);
	if (f.countActiveSources() != 1) return false;
	if (!f.isSourceActive(first)) return false;

	/* An idle source must never be one we are already downloading from. */
	QuickDC::FileSource* second = f.getIdleSource();
	if (!second || second == first) return false;

	f.setSourceActive(first, false);
	return f.countActiveSources() == 0;
});

EXO_TEST(queuefile_expire_spares_active_sources, {
	QuickDC::QueueFile f("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	f.addSource(new QuickDC::FileSource("adc://h:1/BURNT/f"));
	f.addSource(new QuickDC::FileSource("adc://h:1/ALSOBURNT/f"));

	for (size_t n = 0; n < f.countSources(); n++)
		for (int t = 0; t <= QUICKDC_SOURCE_MAX_RETRIES; t++)
			f.getSource(n)->onFailure();

	/* Pin one of them; a source we are mid-transfer with must survive. */
	QuickDC::FileSource* pinned = f.getSource(0);
	f.setSourceActive(pinned, true);

	f.expireSources();
	return f.countSources() == 1 && f.getSource(0) == pinned;
});

/* ------------------------------------------------------------------------- *
 * QueueManager and queue.xml
 *
 * The queue lives at "~/.quickdc/queue.xml", and File::resolvePath() expands
 * that '~' from the environment, so pointing HOME at a scratch directory is
 * all it takes to keep these tests away from the real one.
 * ------------------------------------------------------------------------- */

#define QUEUE_TEST_HOME "/tmp/quickdc-autotest-queue"

static bool queue_test_setup()
{
	if (setenv("HOME", QUEUE_TEST_HOME, 1) != 0) return false;
	Samurai::IO::File::mkdir(QUEUE_TEST_HOME, 0700);
	Samurai::IO::File::mkdir("~/.quickdc", 0700);
	Samurai::IO::File(QUICKDC_QUEUE_FILE).remove();
	return true;
}

EXO_TEST(queue_save_and_load_roundtrip, {
	if (!queue_test_setup()) return false;

	{
		QuickDC::QueueManager queue;
		if (queue.size() != 0) return false;

		QuickDC::QueueFile* a = queue.add("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 2097152);
		if (!a) return false;

		/* Queueing the same local file twice must not duplicate it. */
		if (queue.add("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 2097152) != a) return false;
		if (queue.size() != 1) return false;

		a->setPriority(QuickDC::QueueFile::PrioHigh);
		a->addSource(new QuickDC::FileSource("adc://adchub:7817/ACSTOWQXZASA/TTH:" QUEUE_TEST_TTH_A));
		a->addSource(new QuickDC::FileSource("dchub://myhub:411/NBSWY3DP/file.bin"));
		a->getChunks().addRange(0, 262144);
		a->getChunks().addRange(524288, 1048576);

		/* A name needing XML escaping, to prove save/load are symmetrical. */
		QuickDC::QueueFile* b = queue.add("/tmp/dl/a&b<c>\"d\".iso", QUEUE_TEST_TTH_B, 1000);
		b->setPriority(QuickDC::QueueFile::PrioPaused);

		if (!queue.save()) return false;
	}

	QuickDC::QueueManager queue;
	if (queue.size() != 2) return false;

	QuickDC::QueueFile* a = queue.getFileByName("/tmp/dl/file.bin");
	if (!a) return false;
	if (a->getSize() != 2097152) return false;
	if (!a->getTTH() || strcmp(a->getTTH(), QUEUE_TEST_TTH_A)) return false;
	if (a->getPriority() != QuickDC::QueueFile::PrioHigh) return false;
	if (a->countSources() != 2) return false;
	if (a->getChunks().toString() != "0-262144,524288-1048576") return false;

	/* Nothing can have been in flight in a file that was written to disk. */
	if (a->countActiveSources() != 0) return false;
	if (a->getChunks().getReservationCount() != 0) return false;

	if (!a->findSource("adc://adchub:7817", "ACSTOWQXZASA")) return false;
	if (!a->findSource("dchub://myhub:411", "NBSWY3DP")) return false;

	QuickDC::QueueFile* b = queue.getFileByName("/tmp/dl/a&b<c>\"d\".iso");
	if (!b) return false;

	return b->getPriority() == QuickDC::QueueFile::PrioPaused;
});

EXO_TEST(queue_does_not_store_unresumable_files, {
	if (!queue_test_setup()) return false;

	{
		QuickDC::QueueManager queue;

		/* Persistable. */
		queue.add("/tmp/dl/keep.bin", QUEUE_TEST_TTH_A, 1000);

		/* No TTH: usable now, but could not be re-sourced after a restart. */
		queue.add("/tmp/dl/notth.bin", 0, 500);

		/* Already finished. */
		QuickDC::QueueFile* done = queue.add("/tmp/dl/done.bin", QUEUE_TEST_TTH_B, 100);
		done->getChunks().addRange(0, 100);

		if (queue.size() != 3) return false;
		if (!queue.save()) return false;
	}

	QuickDC::QueueManager queue;
	return queue.size() == 1
		&& queue.getFileByName("/tmp/dl/keep.bin") != 0
		&& queue.getFileByName("/tmp/dl/notth.bin") == 0
		&& queue.getFileByName("/tmp/dl/done.bin") == 0;
});

EXO_TEST(queue_load_drops_bad_entries_and_keeps_good, {
	if (!queue_test_setup()) return false;

	/* 'lastseen' has to be recent, or the source is rightly treated as stale. */
	char xml[2048];
	snprintf(xml, sizeof(xml),
		"<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n"
		"<DownloadQueue version=\"1\">\r\n"
		"\t<File name=\"/tmp/dl/good.bin\" size=\"1000\" tth=\"" QUEUE_TEST_TTH_A "\" added=\"1138411637\" priority=\"low\">\r\n"
		"\t\t<Source url=\"adc://h:1/CID/f\" lastseen=\"%lu\" retries=\"0\" />\r\n"
		"\t\t<Source url=\"garbage\" />\r\n"
		"\t\t<Source url=\"adc://old:1/STALE/f\" lastseen=\"1\" retries=\"0\" />\r\n"
		"\t\t<Chunk start=\"0\" end=\"100\" />\r\n"
		"\t\t<Chunk start=\"900\" end=\"5000\" />\r\n"
		"\t\t<Chunk start=\"50\" end=\"20\" />\r\n"
		"\t</File>\r\n"
		"\t<File name=\"/tmp/dl/shorttth.bin\" size=\"10\" tth=\"TOOSHORT\" />\r\n"
		"\t<File name=\"/tmp/dl/nosize.bin\" size=\"0\" tth=\"" QUEUE_TEST_TTH_B "\" />\r\n"
		"\t<File size=\"10\" tth=\"" QUEUE_TEST_TTH_B "\" />\r\n"
		"</DownloadQueue>\r\n",
		(unsigned long) Samurai::TimeStamp().getInternalData());

	Samurai::IO::File out(QUICKDC_QUEUE_FILE);
	if (!out.open(Samurai::IO::File::Write | Samurai::IO::File::Truncate)) return false;
	out.write(xml, strlen(xml));
	out.close();

	QuickDC::QueueManager queue;
	if (queue.size() != 1) return false;

	QuickDC::QueueFile* good = queue.getFileByName("/tmp/dl/good.bin");
	if (!good) return false;
	if (good->getPriority() != QuickDC::QueueFile::PrioLow) return false;
	if (good->getTimeAdded().getInternalData() != 1138411637) return false;

	/* The unusable and the stale source are dropped; the good one is kept. */
	if (good->countSources() != 1) return false;

	/* Only the chunk that fits inside the file is kept. */
	return good->getChunks().toString() == "0-100";
});

EXO_TEST(queue_lookups, {
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;
	QuickDC::QueueFile* a = queue.add("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	a->addSource(new QuickDC::FileSource("adc://h:1/CID/f"));

	return queue.getFileByTTH(QUEUE_TEST_TTH_A) == a
		&& queue.getFileByTTH(QUEUE_TEST_TTH_B) == 0
		&& queue.getFileByTTH(0) == 0
		&& queue.getFileByName("/tmp/dl/file.bin") == a
		&& queue.getFileByName("/tmp/dl/nope.bin") == 0
		&& queue.getFileBySource("adc://h:1", "CID") == a
		&& queue.getFileBySource("adc://h:1", "OTHER") == 0;
});

EXO_TEST(queue_scheduler_api, {
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;

	QuickDC::QueueFile* normal = queue.add("/tmp/dl/normal.bin", QUEUE_TEST_TTH_A, 4194304);
	normal->addSource(new QuickDC::FileSource("adc://h:1/A/f"));
	normal->addSource(new QuickDC::FileSource("adc://h:1/B/f"));

	QuickDC::QueueFile* urgent = queue.add("/tmp/dl/urgent.bin", QUEUE_TEST_TTH_B, 4194304);
	urgent->setPriority(QuickDC::QueueFile::PrioHigh);
	urgent->addSource(new QuickDC::FileSource("adc://h:1/C/f"));

	QuickDC::QueueFile* paused = queue.add("/tmp/dl/paused.bin", QUEUE_TEST_TTH_B, 1000);
	paused->setPriority(QuickDC::QueueFile::PrioPaused);
	paused->addSource(new QuickDC::FileSource("adc://h:1/D/f"));

	/* Wants more sources: not paused, not complete, under the threshold. */
	std::vector<QuickDC::QueueFile*> needy;
	queue.getFilesNeedingSources(needy, 2);
	if (needy.size() != 1 || needy[0] != urgent) return false;

	/* Highest priority first, and never a paused file. */
	QuickDC::QueueFile* file = 0;
	QuickDC::FileSource* source = 0;
	if (!queue.getNextTransfer(file, source)) return false;
	if (file != urgent || !source) return false;

	/* Reserving marks the source busy and shows up in the transfer count. */
	uint64_t offset = 0;
	uint64_t length = 0;
	if (!queue.reserveChunk(urgent, source, offset, length)) return false;
	if (offset != 0) return false;
	if (urgent->countActiveSources() != 1) return false;
	if (queue.countActiveTransfers() != 1) return false;

	/* With its only source busy, the next transfer moves on to the other file. */
	if (!queue.getNextTransfer(file, source)) return false;
	if (file != normal) return false;

	queue.commitChunk(urgent, offset, length);
	if (urgent->getDoneBytes() != length) return false;

	QuickDC::QueueStatus status;
	if (!queue.getStatus(urgent, status)) return false;
	if (status.size != 4194304 || status.done != length) return false;
	if (status.sources != 1 || status.priority != QuickDC::QueueFile::PrioHigh) return false;
	if (status.complete) return false;

	std::vector<QuickDC::QueueStatus> all;
	queue.getStatus(all);
	return all.size() == 3;
});

EXO_TEST(queue_token_correlation, {
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;
	QuickDC::QueueFile* file = queue.add("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 4194304);
	file->addSource(new QuickDC::FileSource("adc://h:1/CID/f"));

	QuickDC::FileSource* source = file->getIdleSource();
	if (!source) return false;

	const char* created = queue.createRequest(file, source);
	if (!created) return false;
	std::string token(created);

	QuickDC::PendingRequest* request = queue.lookupRequest(token.c_str());
	if (!request) return false;
	if (request->getKind() != QuickDC::PendingRequest::KindFile) return false;
	if (request->getFile() != file) return false;
	if (request->getSource() != source) return false;
	if (request->haveReservation()) return false;

	if (queue.lookupRequest("no-such-token") != 0) return false;

	uint64_t offset = 0;
	uint64_t length = 0;
	if (!queue.reserveChunk(file, source, offset, length)) return false;
	request->setReservation(offset, length);

	/* A failed request hands the chunk back and counts against the source. */
	size_t before = source->getRetryCount();
	queue.onRequestFailed(token.c_str());

	if (queue.lookupRequest(token.c_str()) != 0) return false;
	if (source->getRetryCount() != before + 1) return false;
	if (file->countActiveSources() != 0) return false;
	if (file->getChunks().getReservationCount() != 0) return false;

	/* And a successful one clears the failure count instead. */
	const char* second = queue.createRequest(file, source);
	if (!second) return false;
	std::string secondToken(second);
	queue.onRequestFinished(secondToken.c_str());

	return queue.lookupRequest(secondToken.c_str()) == 0
		&& source->getRetryCount() == 0;
});

EXO_TEST(queue_removing_a_file_drops_its_requests, {
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;
	QuickDC::QueueFile* file = queue.add("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	file->addSource(new QuickDC::FileSource("adc://h:1/CID/f"));

	const char* created = queue.createRequest(file, file->getIdleSource());
	if (!created) return false;
	std::string token(created);

	queue.remove(file);

	/* A dangling request would point at freed memory. */
	return queue.size() == 0 && queue.lookupRequest(token.c_str()) == 0;
});

EXO_TEST(queue_filelists_are_transient, {
	if (!queue_test_setup()) return false;

	{
		QuickDC::QueueManager queue;

		QuickDC::FilelistRequest* list = queue.requestFilelist("adc://h:1", "SOMECID");
		if (!list) return false;
		if (queue.countFilelistRequests() != 1) return false;

		/* Asking twice returns the same request. */
		if (queue.requestFilelist("adc://h:1", "SOMECID") != list) return false;
		if (queue.getFilelistRequest("adc://h:1", "SOMECID") != list) return false;
		if (queue.getFilelistRequest("adc://h:1", "NOBODY") != 0) return false;

		if (!strstr(list->getLocalFilename(), "SOMECID.xml")) return false;
		if (!strstr(list->getTempFilename(), QUICKDC_PART_SUFFIX)) return false;

		const char* created = queue.createRequest(list);
		if (!created) return false;
		std::string token(created);

		QuickDC::PendingRequest* request = queue.lookupRequest(token.c_str());
		if (!request) return false;
		if (request->getKind() != QuickDC::PendingRequest::KindFilelist) return false;
		if (request->getFilelist() != list) return false;

		/* Dropping the list drops the peer request with it. */
		queue.removeFilelistRequest(list);
		if (queue.lookupRequest(token.c_str()) != 0) return false;
		if (queue.countFilelistRequests() != 0) return false;

		/* A file list is never part of the stored queue. */
		queue.requestFilelist("adc://h:1", "ANOTHERCID");
		queue.add("/tmp/dl/real.bin", QUEUE_TEST_TTH_A, 1000);
		if (!queue.save()) return false;
	}

	/* Only the real file came back; the list request did not. */
	QuickDC::QueueManager queue;
	return queue.size() == 1 && queue.countFilelistRequests() == 0;
});

/* ------------------------------------------------------------------------- *
 * NMDC correlation
 *
 * $Direction has nowhere to carry a token, so an NMDC peer connection is
 * matched to its queue job by which user is on the other end.
 * ------------------------------------------------------------------------- */

EXO_TEST(queue_lookup_request_by_source, {
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;

	QuickDC::QueueFile* file = queue.add("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
	file->addSource(new QuickDC::FileSource("dchub://myhub:411/alice/file.bin"));

	QuickDC::FileSource* source = file->getIdleSource();
	if (!source) return false;

	const char* created = queue.createRequest(file, source);
	if (!created) return false;
	std::string token(created);

	/* The whole point: found by user, with no token in hand. */
	QuickDC::PendingRequest* byUser = queue.lookupRequestBySource("dchub://myhub:411", "alice");
	if (!byUser) return false;
	if (byUser != queue.lookupRequest(token.c_str())) return false;
	if (strcmp(byUser->getToken(), token.c_str())) return false;

	/* Must not match a different user, or a different hub. */
	if (queue.lookupRequestBySource("dchub://myhub:411", "bob") != 0) return false;
	if (queue.lookupRequestBySource("dchub://other:411", "alice") != 0) return false;
	if (queue.lookupRequestBySource(0, 0) != 0) return false;

	/* It has to find file list requests too. */
	QuickDC::FilelistRequest* list = queue.requestFilelist("dchub://myhub:411", "carol");
	if (!queue.createRequest(list)) return false;

	QuickDC::PendingRequest* listRequest = queue.lookupRequestBySource("dchub://myhub:411", "carol");
	if (!listRequest) return false;
	if (listRequest->getKind() != QuickDC::PendingRequest::KindFilelist) return false;
	if (listRequest->getFilelist() != list) return false;

	/* And it must stop finding a request once that request is gone. */
	queue.onRequestFinished(token.c_str());
	return queue.lookupRequestBySource("dchub://myhub:411", "alice") == 0;
});

EXO_TEST(nmdc_adcget_file_wire_format, {
	DC::SendADCGet cmd("file", "TTH/" QUEUE_TEST_TTH_A, 1048576, 262144);
	std::string wire(cmd.getRawCommand(), cmd.getSize());
	return wire == "$ADCGET file TTH/" QUEUE_TEST_TTH_A " 1048576 262144|";
});

EXO_TEST(nmdc_adcget_filelist_wire_format, {
	/* A length of -1 means "all of it", so it must not print unsigned. */
	DC::SendADCGet cmd("list", "/", 0, -1);
	std::string wire(cmd.getRawCommand(), cmd.getSize());
	return wire == "$ADCGET list / 0 -1|";
});

EXO_TEST(nmdc_adcget_large_offset, {
	/* Well past 32 bits, which is the whole reason for the 64 bit types. */
	DC::SendADCGet cmd("file", "TTH/" QUEUE_TEST_TTH_B, 5368709120ULL, 1048576);
	std::string wire(cmd.getRawCommand(), cmd.getSize());
	return wire == "$ADCGET file TTH/" QUEUE_TEST_TTH_B " 5368709120 1048576|";
});

/* ------------------------------------------------------------------------- *
 * NMDC $SR parsing
 *
 * 0x05 is the field separator. The path and the hub name may both contain
 * spaces, and the separator count varies between file/directory and
 * active/passive replies, so these cover each shape.
 * ------------------------------------------------------------------------- */

#define SR_SEP "\005"

/** Holds the decoded parts, so each test can stay a single expression. */
struct SRResult {
	std::string nick;
	std::string path;
	std::string tth;
	uint64_t size;
	size_t freeslots;
	bool ok;
};

static SRResult sr_parse(const char* line)
{
	SRResult out;
	out.size = 0;
	out.freeslots = 0;
	out.ok = DC::parseSearchResult(line, strlen(line),
		out.nick, out.path, out.size, out.freeslots, out.tth);
	return out;
}

EXO_TEST(nmdc_sr_active_file_with_hubname, {
	SRResult r = sr_parse("$SR alice share\\movies\\file.bin" SR_SEP
	                      "3145728 2/4" SR_SEP "A QuickDC hub (192.168.0.1:411)");
	return r.ok
		&& r.nick == "alice"
		&& r.path == "share\\movies\\file.bin"
		&& r.size == 3145728
		&& r.freeslots == 2
		&& r.tth.empty();
});

EXO_TEST(nmdc_sr_active_file_with_tth, {
	SRResult r = sr_parse("$SR bob dir\\big.iso" SR_SEP "5368709120 0/3" SR_SEP
	                      "TTH:" QUEUE_TEST_TTH_A " (10.0.0.5:1411)");
	/* A size past 32 bits has to survive, and the TTH is what makes the
	   result usable as a source at all. */
	return r.ok
		&& r.nick == "bob"
		&& r.path == "dir\\big.iso"
		&& r.size == 5368709120ULL
		&& r.freeslots == 0
		&& r.tth == QUEUE_TEST_TTH_A;
});

EXO_TEST(nmdc_sr_passive_file_has_trailing_nick, {
	/* A reply relayed by the hub appends the searcher's nick. */
	SRResult r = sr_parse("$SR carol files\\song.mp3" SR_SEP "4096 1/2" SR_SEP
	                      "TTH:" QUEUE_TEST_TTH_B " (1.2.3.4:411)" SR_SEP "myself");
	return r.ok
		&& r.nick == "carol"
		&& r.path == "files\\song.mp3"
		&& r.size == 4096
		&& r.freeslots == 1
		&& r.tth == QUEUE_TEST_TTH_B;
});

EXO_TEST(nmdc_sr_directory_has_no_size, {
	/* No 0x05 before the slots, because a directory has no size. */
	SRResult r = sr_parse("$SR dave share\\movies 3/4" SR_SEP "A hub (1.2.3.4:411)");
	return r.ok
		&& r.nick == "dave"
		&& r.path == "share\\movies"
		&& r.size == 0
		&& r.freeslots == 3
		&& r.tth.empty();
});

EXO_TEST(nmdc_sr_passive_directory, {
	SRResult r = sr_parse("$SR eve share\\stuff 1/1" SR_SEP "A hub (1.2.3.4:411)" SR_SEP "myself");
	return r.ok && r.path == "share\\stuff" && r.size == 0 && r.freeslots == 1;
});

EXO_TEST(nmdc_sr_spaces_in_path_and_hubname, {
	/* Both fields may contain spaces, which is why the trailer is the anchor. */
	SRResult r = sr_parse("$SR frank my folder\\some long name.bin" SR_SEP
	                      "100 2/2" SR_SEP "The Best Hub Ever (1.2.3.4:411)");
	return r.ok
		&& r.path == "my folder\\some long name.bin"
		&& r.size == 100
		&& r.freeslots == 2;
});

EXO_TEST(nmdc_sr_brackets_in_hubname, {
	/* Only the last '(' opens the address, so a bracketed hub name is fine. */
	SRResult r = sr_parse("$SR gina f.bin" SR_SEP "10 1/1" SR_SEP
	                      "My (cool) hub (1.2.3.4:411)");
	return r.ok && r.path == "f.bin" && r.size == 10 && r.freeslots == 1;
});

EXO_TEST(nmdc_sr_nick_with_spaces_in_path_only, {
	/* A single character nick still parses; the first space ends it. */
	SRResult r = sr_parse("$SR x a b c" SR_SEP "5 1/1" SR_SEP "h (1.2.3.4:411)");
	return r.ok && r.nick == "x" && r.path == "a b c" && r.freeslots == 1;
});

EXO_TEST(nmdc_sr_short_tth_is_ignored, {
	/* Not 39 characters, so it must not be offered as a hash. */
	SRResult r = sr_parse("$SR hank f.bin" SR_SEP "10 1/1" SR_SEP "TTH:TOOSHORT (1.2.3.4:411)");
	return r.ok && r.tth.empty() && r.size == 10 && r.freeslots == 1;
});

EXO_TEST(nmdc_sr_slot_counts, {
	/*
	 * Worth its own case: "{free}/{total}" cannot go through quickdc_atoi(),
	 * which reads "2/4" as -6 because it skips the slash as if it were leading
	 * padding and then keeps going.
	 */
	if (sr_parse("$SR a f" SR_SEP "1 0/3" SR_SEP "h (1.2.3.4:411)").freeslots != 0) return false;
	if (sr_parse("$SR a f" SR_SEP "1 2/4" SR_SEP "h (1.2.3.4:411)").freeslots != 2) return false;
	if (sr_parse("$SR a f" SR_SEP "1 12/40" SR_SEP "h (1.2.3.4:411)").freeslots != 12) return false;

	/* A directory result carries slots too, in the same field as the path. */
	return sr_parse("$SR a some\\dir 7/9" SR_SEP "h (1.2.3.4:411)").freeslots == 7;
});

EXO_TEST(nmdc_sr_rejects_malformed, {
	/* No trailer at all. */
	if (sr_parse("$SR alice f.bin" SR_SEP "10 1/1" SR_SEP "A hub 1.2.3.4:411").ok) return false;

	/* Trailer without a colon is not an address, so nothing anchors the parse. */
	if (sr_parse("$SR alice f.bin" SR_SEP "10 1/1" SR_SEP "A hub (nocolon)").ok) return false;

	/* No slot field. */
	if (sr_parse("$SR alice f.bin" SR_SEP "10 xx" SR_SEP "h (1.2.3.4:411)").ok) return false;

	/* Slot counts that are not numbers. */
	if (sr_parse("$SR alice f.bin" SR_SEP "10 x/4" SR_SEP "h (1.2.3.4:411)").ok) return false;
	if (sr_parse("$SR alice f.bin" SR_SEP "10 2/x" SR_SEP "h (1.2.3.4:411)").ok) return false;
	if (sr_parse("$SR alice f.bin" SR_SEP "10 /4" SR_SEP "h (1.2.3.4:411)").ok) return false;

	/* A size that is not a number. */
	if (sr_parse("$SR alice f.bin" SR_SEP "12x 1/1" SR_SEP "h (1.2.3.4:411)").ok) return false;

	/* No nick. */
	if (sr_parse("$SR  f.bin" SR_SEP "10 1/1" SR_SEP "h (1.2.3.4:411)").ok) return false;

	/* Not a $SR at all. */
	if (sr_parse("$Search alice f.bin 10 1/1 h (1.2.3.4:411)").ok) return false;

	/* Too short to be anything. */
	if (sr_parse("$SR a b").ok) return false;
	if (sr_parse("").ok) return false;

	/* Only the hub trailer, so there is no path. */
	return !sr_parse("$SR alice " SR_SEP "h (1.2.3.4:411)").ok;
});

EXO_TEST(nmdc_sr_result_becomes_a_source, {
	/*
	 * The end to end point of parsing $SR: a reply whose TTH matches something
	 * queued has to be usable as a source, keyed on the nick since NMDC has
	 * no CID.
	 */
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;
	QuickDC::QueueFile* file = queue.add("/tmp/dl/big.iso", QUEUE_TEST_TTH_A, 0);
	if (file->countSources() != 0) return false;

	SRResult r = sr_parse("$SR bob dir\\big.iso" SR_SEP "5368709120 0/3" SR_SEP
	                      "TTH:" QUEUE_TEST_TTH_A " (10.0.0.5:1411)");
	if (!r.ok) return false;

	QuickDC::QueueFile* matched = queue.getFileByTTH(r.tth.c_str());
	if (matched != file) return false;

	/* The reply is also where an unknown size comes from. */
	if (!matched->getSize()) matched->setSize(r.size);
	if (matched->getSize() != 5368709120ULL) return false;

	matched->addSource(new QuickDC::FileSource("dchub://myhub:411", r.nick.c_str(), r.path.c_str()));
	if (matched->countSources() != 1) return false;

	QuickDC::FileSource* source = matched->findSource("dchub://myhub:411", "bob");
	if (!source) return false;

	/* And it round-trips through the queue on disk. */
	char* url = source->toURL();
	bool ok = (url && !strcmp(url, "dchub://myhub:411/bob/dir\\big.iso"));
	free(url);
	return ok;
});

EXO_TEST(queue_filelist_is_schedulable_work, {
	/*
	 * /browse only queues; the queue is what connects. So a fresh request has
	 * to look like work waiting to be started: enumerable, not yet active, and
	 * with nothing in flight for it.
	 */
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;

	QuickDC::FilelistRequest* first = queue.requestFilelist("adc://h:1", "AAA");
	QuickDC::FilelistRequest* second = queue.requestFilelist("adc://h:1", "BBB");
	if (!first || !second) return false;
	if (queue.countFilelistRequests() != 2) return false;

	/* Enumerable in order, and out of range yields 0 rather than misbehaving. */
	if (queue.getFilelistRequest(0) != first) return false;
	if (queue.getFilelistRequest(1) != second) return false;
	if (queue.getFilelistRequest(2) != 0) return false;

	/* Waiting to be started: nothing has asked anyone yet. */
	if (first->isActive()) return false;
	if (queue.lookupRequestBySource("adc://h:1", "AAA") != 0) return false;

	/* Once asked, it must not be picked up a second time. */
	const char* created = queue.createRequest(first);
	if (!created) return false;
	std::string token(created);
	if (queue.lookupRequestBySource("adc://h:1", "AAA") == 0) return false;

	/* The other one is still untouched and schedulable. */
	if (queue.lookupRequestBySource("adc://h:1", "BBB") != 0) return false;

	/* A request that comes to nothing frees the list to be asked again. */
	queue.onRequestFailed(token.c_str());
	if (first->isActive()) return false;
	return queue.lookupRequestBySource("adc://h:1", "AAA") == 0;
});

EXO_TEST(queue_filelist_retry_backs_off, {
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;
	QuickDC::FilelistRequest* list = queue.requestFilelist("adc://h:1", "AAA");
	if (!list) return false;

	/* The first attempt happens immediately. */
	if (!list->isReadyToRetry()) return false;
	if (list->getRetryCount() != 0) return false;

	/*
	 * After a failure it must wait, or the scheduler - which runs several
	 * times a second - would hammer a user who has left the hub and report an
	 * error to the user interface every time.
	 */
	list->onAttempt();
	list->onFailure();
	if (list->getRetryCount() != 1) return false;
	if (list->isReadyToRetry()) return false;

	/* And enough failures take it past the cap, so expire() drops it. */
	for (int n = 0; n < QUICKDC_FILELIST_MAX_RETRIES; n++) list->onFailure();
	return list->getRetryCount() > QUICKDC_FILELIST_MAX_RETRIES;
});

EXO_TEST(usermanager_lookup_by_client_id, {
	/*
	 * The queue stores a client ID, because that is what identifies an ADC user
	 * across sessions. getUser() indexes by session ID instead, so looking a
	 * queued source up needs this - without it the scheduler could never
	 * resolve anyone and no download ever started.
	 */
	QuickDC::UserManager users(0);

	users.join("AAAA");
	users.join("BBBB");

	QuickDC::User* first = (QuickDC::User*) users.getUser("AAAA");
	QuickDC::User* second = (QuickDC::User*) users.getUser("BBBB");
	if (!first || !second) return false;

	/* setClientID() copies a fixed 40 bytes, so these have to be real length
	   CIDs - 39 base32 characters and a terminator. */
	first->setClientID((uint8_t*) QUEUE_TEST_CID_A);
	second->setClientID((uint8_t*) QUEUE_TEST_CID_B);
	first->setNick("alice");

	/* The session ID and the client ID are different keys. */
	if (users.getUserByClientID(QUEUE_TEST_CID_A) != first) return false;
	if (users.getUserByClientID(QUEUE_TEST_CID_B) != second) return false;
	if (users.getUser(QUEUE_TEST_CID_A) != 0) return false;

	/* And a client ID nobody has must not match anyone. */
	if (users.getUserByClientID("NOSUCHCID") != 0) return false;
	if (users.getUserByClientID("") != 0) return false;
	if (users.getUserByClientID(0) != 0) return false;

	/* The nick lookup still works, which is the NMDC path. */
	return users.getUserByNick("alice") == first;
});

/* ------------------------------------------------------------------------- *
 * Remote input handling
 *
 * Every string on a User arrives from a hub or a peer, so none of the lengths
 * are ours to assume.
 * ------------------------------------------------------------------------- */

EXO_TEST(user_long_strings_stay_terminated, {
	QuickDC::User u("AAAA");

	/* 'nick' is 64 bytes; anything at least that long used to be stored with
	   no terminator at all. */
	std::string huge(200, 'n');
	u.setNick(huge.c_str());

	const char* nick = u.getNick();
	if (!nick) return false;
	if (strlen(nick) != 63) return false;
	for (size_t n = 0; n < 63; n++)
		if (nick[n] != 'n') return false;

	/* 'speed' is only 16 bytes, so it is the easiest one to overrun. */
	u.setSpeed(std::string(64, 's').c_str());
	if (strlen(u.getSpeed()) != 15) return false;

	u.setDescription(std::string(1000, 'd').c_str());
	if (strlen(u.getDescription()) != 255) return false;

	u.setEmail(std::string(1000, 'e').c_str());
	return strlen(u.getEmail()) == 127;
});

EXO_TEST(user_long_useragent_does_not_reach_clientid, {
	/*
	 * userAgent is declared immediately before clientID. While a full length
	 * userAgent went in unterminated, reading the client ID ran on into it,
	 * which is what the download queue resolves its sources by.
	 */
	QuickDC::User u("AAAA");
	u.setClientID((uint8_t*) QUEUE_TEST_CID_A);

	u.setUserAgent(std::string(64, 'V').c_str());
	if (strlen(u.getUserAgent()) != 63) return false;

	const char* cid = (const char*) u.getClientID();
	if (!cid) return false;

	/* The client ID has to be exactly what was set, and nothing more. */
	return strcmp(cid, QUEUE_TEST_CID_A) == 0;
});

EXO_TEST(user_short_client_id_is_not_overread, {
	/*
	 * The copy used to take a fixed 40 bytes whatever the argument's length
	 * was, reading past the end of anything shorter. The stored value must be
	 * the string that was passed, terminated.
	 */
	QuickDC::User u("AAAA");

	u.setClientID((uint8_t*) "SHORT");
	if (strcmp((const char*) u.getClientID(), "SHORT")) return false;

	/* A full length CID still round trips. */
	u.setClientID((uint8_t*) QUEUE_TEST_CID_B);
	if (strcmp((const char*) u.getClientID(), QUEUE_TEST_CID_B)) return false;

	/* And a null clears it rather than crashing. */
	u.setClientID(0);
	if (*u.getClientID() != 0) return false;

	u.setPrivateID((uint8_t*) "ALSOSHORT");
	return strcmp((const char*) u.getPrivateID(), "ALSOSHORT") == 0;
});

EXO_TEST(securitymanager_revoke_by_session, {
	/*
	 * revoke(HubSession*) erased while iterating and then incremented the dead
	 * iterator, so it ran off the vector as soon as it matched. The sessions
	 * are only ever compared, never dereferenced, so opaque tags will do.
	 */
	QuickDC::SecurityManager security;

	QuickDC::HubSession* doomed = (QuickDC::HubSession*) 0x1;
	QuickDC::HubSession* keep = (QuickDC::HubSession*) 0x2;

	/* Several consecutive entries for the same session is the case that broke. */
	for (int n = 0; n < 4; n++)
	{
		security.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
			QuickDC::Clearance::ADC, doomed, QUEUE_TEST_CID_A, "doomedtoken"));
	}
	security.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
		QuickDC::Clearance::ADC, keep, QUEUE_TEST_CID_B, "keeptoken"));

	/* Passing no socket keeps the loopback shortcut out of the way. */
	if (!security.isAuthorized(QuickDC::Clearance::ADC, doomed, 0, QUEUE_TEST_CID_A, "doomedtoken"))
		return false;

	security.revoke(doomed);

	/* Every entry for that session is gone, and the other one survived. */
	if (security.isAuthorized(QuickDC::Clearance::ADC, doomed, 0, QUEUE_TEST_CID_A, "doomedtoken"))
		return false;

	return security.isAuthorized(QuickDC::Clearance::ADC, keep, 0, QUEUE_TEST_CID_B, "keeptoken");
});

EXO_TEST(securitymanager_revoke_one_connection, {
	/*
	 * A clearance covers a single connection request. Once that connection has
	 * turned up it must be removed, or the table grows by one entry for every
	 * request the process ever makes.
	 */
	QuickDC::SecurityManager security;

	QuickDC::HubSession* session = (QuickDC::HubSession*) 0x1;

	security.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
		QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "tokenA"));
	security.grant(new QuickDC::Clearance(QuickDC::Clearance::AuthorizedUser,
		QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_B, "tokenB"));

	if (!security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_A, "tokenA"))
		return false;

	/* Consuming one leaves the other alone. */
	if (!security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "tokenA"))
		return false;
	if (security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_A, "tokenA"))
		return false;
	if (!security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_B, "tokenB"))
		return false;

	/* Revoking the same thing twice finds nothing the second time. */
	if (security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "tokenA"))
		return false;

	/* A wrong token or user must not revoke anything. */
	if (security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_B, "wrongtoken"))
		return false;
	if (security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "tokenB"))
		return false;
	if (security.revoke(QuickDC::Clearance::ADC, session, 0, 0))
		return false;

	/* A null session matches any, the way isAuthorized() treats it. */
	if (!security.revoke(QuickDC::Clearance::ADC, 0, QUEUE_TEST_CID_B, "tokenB"))
		return false;

	return !security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_B, "tokenB");
});

EXO_TEST(securitymanager_keeps_blanket_clearance, {
	/*
	 * An UnknownUser clearance is not tied to one connection, so consuming a
	 * connection must not take it away.
	 */
	QuickDC::SecurityManager security;
	QuickDC::HubSession* session = (QuickDC::HubSession*) 0x1;

	security.grant(new QuickDC::Clearance(QuickDC::Clearance::UnknownUser,
		QuickDC::Clearance::ADC, session, 0, 0));

	if (security.revoke(QuickDC::Clearance::ADC, session, QUEUE_TEST_CID_A, "anytoken"))
		return false;

	/* Still open to everyone, as granted. */
	return security.isAuthorized(QuickDC::Clearance::ADC, session, 0, QUEUE_TEST_CID_A, "anytoken");
});

EXO_TEST(queue_empty_saves_and_reloads, {
	if (!queue_test_setup()) return false;

	{
		QuickDC::QueueManager queue;
		QuickDC::QueueFile* file = queue.add("/tmp/dl/file.bin", QUEUE_TEST_TTH_A, 1000);
		if (!queue.save()) return false;
		queue.remove(file);
		if (!queue.save()) return false;
	}

	QuickDC::QueueManager queue;
	return queue.size() == 0;
});
