/*
 * QuickDC::QueueManager: the download queue itself - what it stores in
 * queue.xml and reads back, how it looks a queued file up, what it hands the
 * scheduler, and the peer requests it correlates transfers by.
 */

#include <samurai/samurai.h>
#include <string.h>
#include <stdlib.h>

#include <samurai/io/file.h>
#include <string>
#include <samurai/posix.h>

#include "network/filesource.h"
#include "network/downloadqueue.h"

#define QUEUE_TEST_TTH_A "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define QUEUE_TEST_TTH_B "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

/* ------------------------------------------------------------------------- *
 * QueueManager and queue.xml
 *
 * The queue lives at "~/.quickdc/queue.xml", and File::resolvePath() expands
 * that '~' from the environment, so pointing HOME at a scratch directory is
 * all it takes to keep these tests away from the real one.
 * ------------------------------------------------------------------------- */

/* It carries the process id, because under DISCOVER each case is a process of
   its own - and every one of them removes the queue file on the way in, so a
   home they all shared meant one case deleting the queue another was partway
   through writing. */
static const std::string QUEUE_TEST_HOME =
	"/tmp/quickdc-autotest-queue-" + std::to_string((int) getpid());

static bool queue_test_setup()
{
#ifdef _WIN32
	if (_putenv_s("HOME", QUEUE_TEST_HOME.c_str()) != 0) return false;
#else
	if (setenv("HOME", QUEUE_TEST_HOME.c_str(), 1) != 0) return false;
#endif
	Samurai::IO::File::mkdir(QUEUE_TEST_HOME.c_str(), 0700);
	Samurai::IO::File::mkdir("~/.quickdc", 0700);
	Samurai::IO::File(QUICKDC_QUEUE_FILE).remove();
	return true;
}

namespace {

/*
 * And it goes again when the process does.
 *
 * A name of its own per process is what keeps two cases from treading on each
 * other, and it is also what would leave a directory behind for every case in
 * the suite. A destructor rather than a line at the end of each test, because a
 * case that fails returns early and would skip one.
 */
struct QueueTestHome
{
	~QueueTestHome()
	{
		Samurai::IO::File(QUICKDC_QUEUE_FILE).remove();
		Samurai::IO::File(QUICKDC_QUEUE_FILE_NEW).remove();
		Samurai::IO::File::rmdir((QUEUE_TEST_HOME + "/.quickdc").c_str());
		Samurai::IO::File::rmdir(QUEUE_TEST_HOME.c_str());
	}
};

QueueTestHome queue_test_home;

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
	if (!out.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate)) return false;
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

EXO_TEST(queue_refuses_a_filelist_that_would_escape_the_list_directory, {
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;

	/* A hub advertising this nickname plus one /browse used to be a write to
	   wherever it pointed. */
	if (queue.requestFilelist("dchub://h:411", "../../../../tmp/quickdc_traversal") != 0)
		return false;
	if (queue.requestFilelist("dchub://h:411", "a/b") != 0) return false;
	if (queue.countFilelistRequests() != 0) return false;

	/* And an ordinary nickname is still perfectly askable. */
	QuickDC::FilelistRequest* list = queue.requestFilelist("dchub://h:411", "alice");
	if (!list || !list->isValid()) return false;
	if (!strstr(list->getLocalFilename(), "/alice.xml")) return false;
	return queue.countFilelistRequests() == 1;
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
