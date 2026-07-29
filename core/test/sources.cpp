/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Where a download gets its sources from: what a QueueFile does with the peers
 * it is offered, and what mining a downloaded file list turns up.
 *
 * collectSources() walks a peer's file list looking for anything queued,
 * matched on TTH rather than on name, and is the reason for asking for a file
 * list at all. It found nothing at all until recently: RemoteFile never added
 * itself to its directory, so the walk saw only empty directories. Nothing
 * noticed, because nothing tested it.
 *
 * QueueManager and FilelistRequest both read and write their own files, so
 * every case here points them at scratch paths rather than at the real ones.
 */

#include "quickdc.h"
#include "network/downloadqueue.h"
#include "network/filesource.h"

#include <samurai/io/file.h>
#include <samurai/io/compression.h>
#include <samurai/io/codec.h>

#include <memory>
#include <span>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define TTH1 "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define TTH2 "AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define TTH3 "BBXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"

#define HUB "adc://hub.example:412"
#define CID "AAAABBBBCCCCDDDD"

/* Scratch paths, named by counter so a run is reproducible. */
static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_src_test_" + std::to_string(counter++) + "_" + leaf;
}

static bool write_file(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) return false;
	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

static std::string bzip2(const std::string& plain)
{
	Samurai::IO::BZip2Compressor codec;
	std::string out;
	std::span<const char> in(plain);
	char buf[65536];

	while (true)
	{
		const Samurai::IO::Codec::Progress p = codec.step(in, buf);
		if (p.status == Samurai::IO::Codec::Status::Error) return std::string();
		out.append(buf, p.produced);
		in = in.subspan(p.consumed);
		if (p.status == Samurai::IO::Codec::Status::StreamEnd) break;
		if (!p.consumed && !p.produced) break;
	}
	return out;
}

/* ------------------------------------------------------------------------- */
/* What a QueueFile does with the sources it is offered                      */
/* ------------------------------------------------------------------------- */

static void test_sources()
{
	/* A source is taken over, and asking for it back finds it. */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, CID));

		CHECK(file.countSources() == 1);
		CHECK(file.findSource(HUB, CID) != 0);
		CHECK(file.findSource(HUB, "SOMEONEELSE") == 0);
		CHECK(file.findSource("adc://other:412", CID) == 0);
		printf("  ok source is taken over and found again\n");
	}

	/*
	 * The same peer twice keeps the one already held, because that is the one
	 * carrying the retry count and the last-seen time.
	 */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, CID));

		QuickDC::FileSource* first = file.getSource(0);
		first->setRetryCount(3);

		file.addSource(new QuickDC::FileSource(HUB, CID));

		CHECK(file.countSources() == 1);
		CHECK(file.getSource(0) == first);
		CHECK(file.getSource(0)->getRetryCount() == 3);
		printf("  ok a duplicate source keeps the history\n");
	}

	/* An unusable url is dropped rather than stored. */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource("not a url"));
		file.addSource(0);

		CHECK(file.countSources() == 0);
		printf("  ok an unusable source is dropped\n");
	}

	/* Several distinct peers all stay. */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));
		file.addSource(new QuickDC::FileSource(HUB, "BBBB"));
		file.addSource(new QuickDC::FileSource("adc://other:412", "AAAA"));

		CHECK(file.countSources() == 3);
		printf("  ok distinct sources all stay\n");
	}

	/* An idle source is one not in flight and not expired. */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));
		file.addSource(new QuickDC::FileSource(HUB, "BBBB"));

		QuickDC::FileSource* first = file.getSource(0);
		QuickDC::FileSource* second = file.getSource(1);

		CHECK(file.getIdleSource() == first);
		CHECK(file.countActiveSources() == 0);

		file.setSourceActive(first, true);
		CHECK(file.isSourceActive(first));
		CHECK(file.countActiveSources() == 1);
		CHECK(file.getIdleSource() == second);

		file.setSourceActive(second, true);
		CHECK(file.countActiveSources() == 2);
		CHECK(file.getIdleSource() == 0);

		file.setSourceActive(first, false);
		CHECK(file.getIdleSource() == first);
		printf("  ok an idle source is neither in flight nor expired\n");
	}

	/* A source that failed too often is passed over, and then forgotten. */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));
		file.addSource(new QuickDC::FileSource(HUB, "BBBB"));

		file.getSource(0)->setRetryCount(QUICKDC_SOURCE_MAX_RETRIES + 1);
		CHECK(file.getSource(0)->isExpired());

		QuickDC::FileSource* good = file.getSource(1);
		CHECK(file.getIdleSource() == good);

		file.expireSources();
		CHECK(file.countSources() == 1);
		CHECK(file.getSource(0) == good);
		printf("  ok an exhausted source is passed over and expired\n");
	}

	/* A peer not seen for a very long time has gone stale. */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));

		file.getSource(0)->setLastSeen(Samurai::TimeStamp((time_t) 1000));
		CHECK(file.getSource(0)->isExpired());
		CHECK(file.getIdleSource() == 0);

		file.expireSources();
		CHECK(file.countSources() == 0);
		printf("  ok a source not seen for years has gone stale\n");
	}

	/* One being downloaded from is never expired out from under the transfer. */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));

		QuickDC::FileSource* source = file.getSource(0);
		source->setRetryCount(QUICKDC_SOURCE_MAX_RETRIES + 1);
		file.setSourceActive(source, true);

		file.expireSources();
		CHECK(file.countSources() == 1);
		printf("  ok an active source is not expired mid-transfer\n");
	}

	/* Removing one takes it out of the active set too. */
	{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));

		QuickDC::FileSource* source = file.getSource(0);
		file.setSourceActive(source, true);
		file.removeSource(source);

		CHECK(file.countSources() == 0);
		CHECK(file.countActiveSources() == 0);
		CHECK(file.getSource(0) == 0);
		printf("  ok removing a source clears it from the active set\n");
	}

	/* A url round trips through the source. */
	{
		QuickDC::FileSource source("adc://hub.example:412/AAAABBBB/TTH:" TTH1);
		CHECK(source.isValid());
		CHECK(source.getHubUrl() && !strcmp(source.getHubUrl(), "adc://hub.example:412"));
		CHECK(source.getClientID() && !strcmp(source.getClientID(), "AAAABBBB"));
		printf("  ok a url decodes into hub and client id\n");
	}
}

/* ------------------------------------------------------------------------- */
/* Mining a downloaded file list for sources                                 */
/* ------------------------------------------------------------------------- */

#define LIST(body) \
	"<?xml version=\"1.1\" encoding=\"utf-8\"?>\r\n" \
	"<FileListing Version=\"1\" Base=\"/\">\r\n" body "</FileListing>\r\n"

/*
 * Puts 'content' where a finished download would have left it and mines it,
 * returning how many sources it gave the queue.
 */
static size_t mine(QuickDC::QueueManager& queue, const std::string& dir,
                   const std::string& content)
{
	QuickDC::FilelistRequest request(HUB, CID, dir.c_str());

	if (!write_file(request.getTempFilename(), content)) return (size_t) -1;

	const size_t added = queue.onFilelistDownloaded(&request, HUB, CID);

	Samurai::IO::File(std::string(request.getTempFilename())).remove();
	Samurai::IO::File(std::string(request.getLocalFilename())).remove();
	return added;
}

/* A queue holding the three TTHs above, in its own scratch file. */
class Fixture
{
	public:
		Fixture()
			: dir(scratch("lists")), queue_path(scratch("queue.xml"))
		{
			Samurai::IO::File::mkdir(dir.c_str());
			queue = std::make_unique<QuickDC::QueueManager>(queue_path.c_str());
		}

		~Fixture()
		{
			/* It writes the queue out on the way down, so it has to be gone
			   before there is any point removing the file. */
			queue.reset();

			Samurai::IO::File::rmdir(dir.c_str());
			Samurai::IO::File(queue_path).remove();
			Samurai::IO::File(queue_path + ".new").remove();
		}

		void want(const char* name, const char* tth, uint64_t size = 1000)
		{
			queue->add(name, tth, size);
		}

		size_t mineList(const std::string& content) { return mine(*queue, dir, content); }

		std::string dir;
		std::string queue_path;
		std::unique_ptr<QuickDC::QueueManager> queue;
};

static void test_collect()
{
	/* The point of the whole exercise: a queued TTH found in a peer's list. */
	{
		Fixture fix;
		fix.want("/tmp/wanted.bin", TTH1);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"theirname.bin\" Size=\"1000\" TTH=\"" TTH1 "\"/>"));

		CHECK(added == 1);

		QuickDC::QueueFile* file = fix.queue->getFileByTTH(TTH1);
		CHECK(file != 0);
		CHECK(file && file->countSources() == 1);
		CHECK(file && file->findSource(HUB, CID) != 0);
		printf("  ok a queued tth in a peer's list becomes a source\n");
	}

	/* Matched on the TTH; what the peer calls it does not matter. */
	{
		Fixture fix;
		fix.want("/tmp/mine.bin", TTH1);

		fix.mineList(LIST(
			"<File Name=\"totally different name.bin\" Size=\"1000\" TTH=\"" TTH1 "\"/>"));

		QuickDC::QueueFile* file = fix.queue->getFileByTTH(TTH1);
		CHECK(file && file->countSources() == 1);

		/* The peer's name is what we would have to ask for. */
		CHECK(file && file->getSource(0)->getName()
			&& !strcmp(file->getSource(0)->getName(), "totally different name.bin"));
		printf("  ok the match is on the tth, not the name\n");
	}

	/* Nothing queued matches, so nothing is added. */
	{
		Fixture fix;
		fix.want("/tmp/wanted.bin", TTH1);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"other.bin\" Size=\"1000\" TTH=\"" TTH2 "\"/>"));

		CHECK(added == 0);
		CHECK(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
		printf("  ok a list with nothing we want adds nothing\n");
	}

	/* Files nested in directories are reached. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);
		fix.want("/tmp/b.bin", TTH2);

		const size_t added = fix.mineList(LIST(
			"<Directory Name=\"music\">"
			"<File Name=\"a.bin\" Size=\"1000\" TTH=\"" TTH1 "\"/>"
			"<Directory Name=\"deeper\">"
			"<File Name=\"b.bin\" Size=\"1000\" TTH=\"" TTH2 "\"/>"
			"</Directory>"
			"</Directory>"));

		CHECK(added == 2);
		printf("  ok the walk descends into directories\n");
	}

	/* Several wanted files in one list. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);
		fix.want("/tmp/b.bin", TTH2);
		fix.want("/tmp/c.bin", TTH3);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>"
			"<File Name=\"x\" Size=\"1000\" TTH=\"XXXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A\"/>"
			"<File Name=\"b\" Size=\"1000\" TTH=\"" TTH2 "\"/>"));

		CHECK(added == 2);
		CHECK(fix.queue->getFileByTTH(TTH3)->countSources() == 0);
		printf("  ok several wanted files are all picked up\n");
	}

	/* The same peer offered twice counts once. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		CHECK(fix.mineList(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>")) == 1);

		/* Asking the same peer again tells us nothing new. */
		CHECK(fix.mineList(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>")) == 0);

		CHECK(fix.queue->getFileByTTH(TTH1)->countSources() == 1);
		printf("  ok a source we already had is not counted twice\n");
	}

	/* The same file listed twice by one peer is still one source. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"copy1\" Size=\"1000\" TTH=\"" TTH1 "\"/>"
			"<Directory Name=\"elsewhere\">"
			"<File Name=\"copy2\" Size=\"1000\" TTH=\"" TTH1 "\"/>"
			"</Directory>"));

		CHECK(fix.queue->getFileByTTH(TTH1)->countSources() == 1);
		CHECK(added <= 2);
		printf("  ok one peer listing a file twice is one source\n");
	}

	/* A bzip2 file list, which is what most peers actually send. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const std::string plain = LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>");
		const std::string packed = bzip2(plain);
		CHECK(!packed.empty());
		CHECK(packed.compare(0, 3, "BZh") == 0);

		CHECK(fix.mineList(packed) == 1);
		printf("  ok a bzip2 file list is unpacked and mined\n");
	}

	/* Entries with no TTH cannot be matched and must not crash the walk. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"notth\" Size=\"1000\"/>"
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>"));

		CHECK(added == 1);
		printf("  ok an entry without a tth is skipped\n");
	}

	/* Nothing usable in the file at all. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		CHECK(fix.mineList(LIST("")) == 0);
		CHECK(fix.mineList("this is not a file list") == 0);
		CHECK(fix.mineList("BZhthis is not bzip2 either") == 0);
		printf("  ok an empty or unusable list adds nothing\n");
	}

	/* A request whose download never arrived. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		QuickDC::FilelistRequest request(HUB, CID, fix.dir.c_str());
		CHECK(fix.queue->onFilelistDownloaded(&request, HUB, CID) == 0);
		CHECK(fix.queue->onFilelistDownloaded(0, HUB, CID) == 0);
		printf("  ok a missing download adds nothing\n");
	}

	/* An empty queue has nothing to match against. */
	{
		Fixture fix;
		CHECK(fix.mineList(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>")) == 0);
		printf("  ok a list mined against an empty queue adds nothing\n");
	}
}

int main()
{
	setbuf(stdout, 0);

	test_sources();
	test_collect();

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
