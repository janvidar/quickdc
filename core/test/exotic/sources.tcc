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

/*
 * A bzip2 stream of 'block' repeated, built without ever holding the plaintext.
 * The bomb cases need a stream that unpacks to far more than anything anyone
 * would want in memory, which is the whole point of them.
 */
static std::string bzip2_repeated(const std::string& block, size_t times)
{
	Samurai::IO::BZip2Compressor codec;
	std::string out;
	char buf[65536];

	std::span<const char> in;
	size_t fed = 0;
	bool flushing = false;

	while (true)
	{
		if (in.empty() && !flushing)
		{
			if (fed < times)
			{
				in = std::span<const char>(block);
				fed++;
			}
			else
			{
				flushing = true;   /* an empty input asks it to flush */
			}
		}

		const Samurai::IO::Codec::Progress p = codec.step(in, buf);
		if (p.status == Samurai::IO::Codec::Status::Error) return std::string();

		out.append(buf, p.produced);
		in = in.subspan(p.consumed);

		if (p.status == Samurai::IO::Codec::Status::StreamEnd) break;
		if (flushing && !p.produced) return std::string();
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

/* The same document as LIST(), for a body that is not a literal. */
static std::string wrap_list(const std::string& body)
{
	return std::string("<?xml version=\"1.1\" encoding=\"utf-8\"?>\r\n"
		"<FileListing Version=\"1\" Base=\"/\">\r\n") + body + "</FileListing>\r\n";
}

/* A file list with one wanted file at the bottom of 'depth' directories. */
static std::string nested_list(size_t depth)
{
	std::string body;
	for (size_t n = 0; n < depth; n++) body += "<Directory Name=\"d\">";
	body += "<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>";
	for (size_t n = 0; n < depth; n++) body += "</Directory>";

	return wrap_list(body);
}

/*
 * A file list of 'entries' files, all of them the one we want. Long enough that
 * bzip2 splits it across several blocks, which is what makes a truncated copy
 * of it decode into a list that reads as complete and is not.
 */
static std::string big_list(size_t entries)
{
	std::string body;
	for (size_t n = 0; n < entries; n++)
	{
		body += "<File Name=\"f";
		body += std::to_string(n);
		body += "\" Size=\"1000\" TTH=\"" TTH1 "\"/>";
	}
	return wrap_list(body);
}

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

/* ------------------------------------------------------------------------- */
/* What a file list request is allowed to name                               */
/* ------------------------------------------------------------------------- */

/*
 * The identifier a list is named after is a CID only when the hub published one
 * and it verified. On NMDC it is always the nickname, which is 63 arbitrary
 * bytes chosen by whoever is on the hub - so it decides a path, and a hub that
 * names a user "../../../.config/autostart/x" would otherwise get a file
 * written wherever it likes with content of its choosing.
 */
static void test_filelist_naming()
{
	const std::string dir = scratch("lists");
	const std::string prefix = dir + "/";

	/* An ADC CID and an ordinary nickname both still name a file in there. */
	{
		QuickDC::FilelistRequest byCid(HUB, CID, dir.c_str());
		CHECK(byCid.isValid());
		CHECK(byCid.getLocalFilename()
			&& byCid.getLocalFilename() == prefix + CID + ".xml");
		CHECK(byCid.getTempFilename()
			&& byCid.getTempFilename() == prefix + CID + ".xml" QUICKDC_PART_SUFFIX);

		QuickDC::FilelistRequest byNick("dchub://hub.example:411", "[NL]alice", dir.c_str());
		CHECK(byNick.isValid());
		CHECK(byNick.getLocalFilename()
			&& byNick.getLocalFilename() == prefix + "[NL]alice.xml");
		printf("  ok a cid and a nickname both name a file in the list directory\n");
	}

	/* None of these may become a path. */
	static const char* hostile[] = {
		"../../../../tmp/quickdc_traversal",
		"..",
		".",
		"a/b",
		"a\\b",
		"/etc/passwd",
		"",
		0
	};

	for (size_t n = 0; hostile[n]; n++)
	{
		QuickDC::FilelistRequest request(HUB, hostile[n], dir.c_str());

		CHECK(!request.isValid());
		CHECK(request.getLocalFilename() == 0);
		CHECK(request.getTempFilename() == 0);

		/*
		 * And if one were ever accepted, what it names still has to sit inside
		 * the directory it was given - which is the property that matters
		 * whatever the identifier turns out to contain.
		 */
		if (request.getLocalFilename())
		{
			const std::string resolved =
				Samurai::IO::File::resolvePath(request.getLocalFilename());
			CHECK(resolved.compare(0, prefix.size(), prefix) == 0);
		}
	}
	printf("  ok an identifier that would name a path is refused\n");

	/* The queue refuses to hold one, so no download is ever started for it. */
	{
		Fixture fix;

		CHECK(fix.queue->requestFilelist(HUB, "../../../../tmp/quickdc_traversal") == 0);
		CHECK(fix.queue->requestFilelist(HUB, "a/b") == 0);
		CHECK(fix.queue->countFilelistRequests() == 0);

		CHECK(fix.queue->requestFilelist(HUB, CID) != 0);
		CHECK(fix.queue->countFilelistRequests() == 1);
		printf("  ok the queue refuses a request it cannot name a file for\n");
	}
}

/* ------------------------------------------------------------------------- */
/* What a downloaded file list is allowed to cost us                         */
/* ------------------------------------------------------------------------- */

/*
 * Everything here arrives from a peer, so both what turns up and what it
 * unpacks to are bounded. bzip2 reaches about a million to one on input crafted
 * for it, and a list is unpacked before anything has looked at it.
 */
static void test_filelist_limits()
{
	/*
	 * Far more than any share could account for, and refused on its size before
	 * anything unpacks it - so the perfectly good list at the front of it is
	 * never reached either.
	 */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		std::string oversized = bzip2(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>"));
		CHECK(!oversized.empty());
		oversized.append((size_t) QUICKDC_FILELIST_MAX_PACKED, '\0');

		CHECK(fix.mineList(oversized) == 0);
		CHECK(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
		printf("  ok an oversized compressed file list is refused\n");
	}

	/* A bomb: a small stream that unpacks to more than the limit. */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const std::string block(1024 * 1024, '\0');
		const size_t blocks = (QUICKDC_FILELIST_MAX_PLAIN / (1024 * 1024)) + 16;

		const std::string bomb = bzip2_repeated(block, blocks);
		CHECK(!bomb.empty());
		CHECK(bomb.size() < 64 * 1024);   /* which is what makes it a bomb */

		QuickDC::FilelistRequest request(HUB, CID, fix.dir.c_str());
		CHECK(write_file(request.getTempFilename(), bomb));

		CHECK(fix.queue->onFilelistDownloaded(&request, HUB, CID) == 0);

		/*
		 * And nothing of it is left behind: a list that is short of entries but
		 * reads as complete would have us decide a peer holds nothing we want.
		 */
		CHECK(!Samurai::IO::File(std::string(request.getLocalFilename())).exists());

		Samurai::IO::File(std::string(request.getTempFilename())).remove();
		printf("  ok a decompression bomb is refused and leaves nothing behind\n");
	}

	/*
	 * A stream that stops short. The blocks before the cut unpack perfectly well
	 * and the reader is forgiving about the tag that never closes, so what comes
	 * out reads as a complete list of a peer who holds less than they do - which
	 * is worse than no list at all, and so is refused.
	 */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const std::string packed = bzip2(big_list(40000));
		CHECK(packed.size() > 1000);

		const std::string truncated = packed.substr(0, packed.size() - packed.size() / 8);

		QuickDC::FilelistRequest request(HUB, CID, fix.dir.c_str());
		CHECK(write_file(request.getTempFilename(), truncated));

		CHECK(fix.queue->onFilelistDownloaded(&request, HUB, CID) == 0);
		CHECK(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
		CHECK(!Samurai::IO::File(std::string(request.getLocalFilename())).exists());

		Samurai::IO::File(std::string(request.getTempFilename())).remove();
		printf("  ok a truncated compressed file list is refused\n");
	}

	/*
	 * Nesting. The tree is built and then walked recursively, and nothing in the
	 * format stops a peer nesting a directory inside itself until our stack runs
	 * out, so a document past the limit is refused rather than followed.
	 */
	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		/* Well within the limit: still walked to the bottom. */
		CHECK(fix.mineList(nested_list(100)) == 1);
		printf("  ok ordinary nesting is walked to the bottom\n");
	}

	{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		CHECK(fix.mineList(nested_list(QUICKDC_FILELIST_MAX_DEPTH + 64)) == 0);
		CHECK(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
		printf("  ok a file list nested past the limit is refused\n");
	}
}

/*
 * How much a source is asked for at a time.
 *
 * A chunk costs a round trip - it is not asked for until the one before it has
 * arrived and been reported - so a fixed size that suits a slow peer leaves a
 * fast one idle between chunks, and its rate sawtooths. The size follows what
 * the source has actually been managing instead.
 *
 * There is no Core here, so getChunkSize() answers with the built-in default
 * rather than with anything configured.
 */
static void test_chunk_sizing()
{
	Fixture fix;

	/* Nothing measured yet, and nothing to measure. */
	QuickDC::FileSource unknown(HUB, CID, "/tmp/a.bin");
	CHECK(unknown.getThroughput() == 0);
	CHECK(fix.queue->chunkSizeFor(&unknown) == QUICKDC_CHUNK_SIZE);
	CHECK(fix.queue->chunkSizeFor(0) == QUICKDC_CHUNK_SIZE);
	printf("  ok a source we know nothing about is asked for the default\n");

	/* Slow: two seconds of it is less than the floor, so the floor stands. */
	QuickDC::FileSource slow(HUB, CID, "/tmp/b.bin");
	slow.onChunkDone(64 * 1024, 1.0);
	CHECK(slow.getThroughput() == 64 * 1024);
	CHECK(fix.queue->chunkSizeFor(&slow) == QUICKDC_CHUNK_SIZE);
	printf("  ok a slow source is never asked for less than the default\n");

	/* Quick: about QUICKDC_CHUNK_TARGET_SECONDS of transfer. */
	QuickDC::FileSource quick(HUB, CID, "/tmp/c.bin");
	quick.onChunkDone(4 * 1024 * 1024, 1.0);
	CHECK(quick.getThroughput() == 4u * 1024 * 1024);
	CHECK(fix.queue->chunkSizeFor(&quick)
		== 4u * 1024 * 1024 * QUICKDC_CHUNK_TARGET_SECONDS);
	printf("  ok a quick source is asked for a couple of seconds' worth\n");

	/*
	 * And a very fast one is held at the ceiling. Twenty five megabytes a second
	 * is where a megabyte chunk was being asked for twenty five times over.
	 */
	QuickDC::FileSource fast(HUB, CID, "/tmp/d.bin");
	for (int n = 0; n < 20; n++) fast.onChunkDone(25 * 1024 * 1024, 1.0);
	CHECK(fast.getThroughput() > 20u * 1024 * 1024);
	CHECK(fix.queue->chunkSizeFor(&fast) == (uint64_t) QUICKDC_CHUNK_SIZE_MAX);
	printf("  ok however fast the source, a chunk stops at the ceiling\n");

	/* Smoothed, so one slow chunk does not collapse the next request. */
	const uint64_t settled = fast.getThroughput();
	fast.onChunkDone(1024, 1.0);
	CHECK(fast.getThroughput() > settled / 2);
	printf("  ok one slow chunk does not undo what a source has shown\n");

	/* Nothing arriving, or arriving in no time at all, measures nothing. */
	QuickDC::FileSource odd(HUB, CID, "/tmp/e.bin");
	odd.onChunkDone(0, 1.0);
	odd.onChunkDone(1024, 0.0);
	odd.onChunkDone(1024, -1.0);
	CHECK(odd.getThroughput() == 0);
	printf("  ok a chunk of nothing, or in no time, is not a measurement\n");
}
