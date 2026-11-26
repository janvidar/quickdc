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
#include <samurai/posix.h>

#define TTH1 "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define TTH2 "AAXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define TTH3 "BBXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"

#define HUB "adc://hub.example:412"
#define CID "AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHHIIIIJJJ"

/*
 * Scratch paths, named by process as well as by counter.
 *
 * The counter alone is not enough to tell two of these apart: every case in this
 * file is registered as a CTest entry of its own and so runs as its own process,
 * where a static counter starts again at zero - so a parallel run had two cases
 * writing, reading and unlinking the same path at once.
 */
static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_src_test_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
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

/*
 * A queue whose source rotation can be looked at.
 *
 * What the rotation is made of - when it last searched, and whose turn is next -
 * is protected, and the property that matters about searching for one file on
 * demand is precisely that it leaves both of them alone. A subclass is how a
 * test gets to see that, and it is also how searchForSources() can be run at a
 * moment of the case's choosing rather than whenever process() feels like it.
 */
class PeekableQueue : public QuickDC::QueueManager
{
	public:
		PeekableQueue(const char* path) : QuickDC::QueueManager(path) { }

		/** How long the rotation has been waiting for its turn. */
		time_t sinceRotation() { return lastSourceSearch.elapsed(); }

		/** Which of the files short of sources is next. */
		size_t rotation() const { return searchRotation; }

		void rotate() { searchForSources(); }
};

/* A queue holding the three TTHs above, in its own scratch file. */
class Fixture
{
	public:
		Fixture()
			: dir(scratch("lists")), queue_path(scratch("queue.xml"))
		{
			Samurai::IO::File::mkdir(dir.c_str());
			queue = std::make_unique<PeekableQueue>(queue_path.c_str());
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
		std::unique_ptr<PeekableQueue> queue;
};

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

/* ------------------------------------------------------------------------- */
/* What a downloaded file list is allowed to cost us                         */
/* ------------------------------------------------------------------------- */

/*
 * Everything here arrives from a peer, so both what turns up and what it
 * unpacks to are bounded. bzip2 reaches about a million to one on input crafted
 * for it, and a list is unpacked before anything has looked at it.
 */

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

/* A source is taken over, and asking for it back finds it. */
EXO_TEST(sources_source_is_taken_over_and_found_again,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, CID));

		EXO_ASSERT(file.countSources() == 1);
		EXO_ASSERT(file.findSource(HUB, CID) != 0);
		EXO_ASSERT(file.findSource(HUB, "SOMEONEELSEAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == 0);
		EXO_ASSERT(file.findSource("dchub://hub.example:412", CID) == 0);
	
	return 1;
});

/*
	 * The same peer twice keeps the one already held, because that is the one
	 * carrying the retry count and the last-seen time.
	 */
EXO_TEST(sources_a_duplicate_source_keeps_the_history,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, CID));

		QuickDC::FileSource* first = file.getSource(0);
		first->setRetryCount(3);

		file.addSource(new QuickDC::FileSource(HUB, CID));

		EXO_ASSERT(file.countSources() == 1);
		EXO_ASSERT(file.getSource(0) == first);
		EXO_ASSERT(file.getSource(0)->getRetryCount() == 3);
	
	return 1;
});

/* An unusable url is dropped rather than stored. */
EXO_TEST(sources_an_unusable_source_is_dropped,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource("not a url"));
		file.addSource(0);

		EXO_ASSERT(file.countSources() == 0);
	
	return 1;
});

/* Several distinct peers all stay. */
EXO_TEST(sources_distinct_sources_all_stay,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));
		file.addSource(new QuickDC::FileSource(HUB, "BBBB"));
		file.addSource(new QuickDC::FileSource("dchub://one:411", "carol"));
		file.addSource(new QuickDC::FileSource("dchub://two:411", "carol"));

		EXO_ASSERT(file.countSources() == 4);
	
	return 1;
});

/*
 * One peer is one source however it was learned: a CID found on a second ADC
 * hub, or a hub's address written another way. Two sources for one peer were
 * two connections to it fetching the same file, each throwing away the
 * other's leaves.
 */
EXO_TEST(sources_one_peer_is_one_source_however_it_was_learned,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, CID));
		file.addSource(new QuickDC::FileSource("adcs://elsewhere.example:5000", CID));
		file.addSource(new QuickDC::FileSource("ADC://Hub.Example:412", CID));
		file.addSource(new QuickDC::FileSource("dchub://nmdc.example", "dave"));
		file.addSource(new QuickDC::FileSource("dchub://NMDC.example:411", "dave"));
		file.addSource(new QuickDC::FileSource(" dchub://nmdc.example:411", "dave", "TTH:" TTH1));

		EXO_ASSERT(file.countSources() == 2);
		EXO_ASSERT(file.findSource("adc://elsewhere.example:5000", CID) == file.getSource(0));
		EXO_ASSERT(!strcmp(file.getSource(1)->getHubUrl(), "dchub://nmdc.example:411"));
	
	return 1;
});

/* An idle source is one not in flight and not expired. */
EXO_TEST(sources_an_idle_source_is_neither_in_flight_nor_expired,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));
		file.addSource(new QuickDC::FileSource(HUB, "BBBB"));

		QuickDC::FileSource* first = file.getSource(0);
		QuickDC::FileSource* second = file.getSource(1);

		EXO_ASSERT(file.getIdleSource() == first);
		EXO_ASSERT(file.countActiveSources() == 0);

		file.setSourceActive(first, true);
		EXO_ASSERT(file.isSourceActive(first));
		EXO_ASSERT(file.countActiveSources() == 1);
		EXO_ASSERT(file.getIdleSource() == second);

		file.setSourceActive(second, true);
		EXO_ASSERT(file.countActiveSources() == 2);
		EXO_ASSERT(file.getIdleSource() == 0);

		file.setSourceActive(first, false);
		EXO_ASSERT(file.getIdleSource() == first);
	
	return 1;
});

/* A source that failed too often is passed over, and then forgotten. */
EXO_TEST(sources_an_exhausted_source_is_passed_over_and_expired,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));
		file.addSource(new QuickDC::FileSource(HUB, "BBBB"));

		file.getSource(0)->setRetryCount(QUICKDC_SOURCE_MAX_RETRIES + 1);
		EXO_ASSERT(file.getSource(0)->isExpired());

		QuickDC::FileSource* good = file.getSource(1);
		EXO_ASSERT(file.getIdleSource() == good);

		file.expireSources();
		EXO_ASSERT(file.countSources() == 1);
		EXO_ASSERT(file.getSource(0) == good);
	
	return 1;
});

/*
 * A user on no hub we are on is left alone for the retry interval - not asked for
 * on every pass - but that is not a failure: however often it happens, it does not
 * bring the source any nearer to being given up on.
 */
EXO_TEST(sources_an_absent_user_is_waited_for_without_being_counted_against,
{
		QuickDC::FileSource source(HUB, "AAAA");
		EXO_ASSERT(source.isReadyToRetry());

		for (int n = 0; n < QUICKDC_SOURCE_MAX_RETRIES * 2; n++) source.onAbsent();

		EXO_ASSERT(!source.isReadyToRetry());
		EXO_ASSERT_EQ_UINT(source.getRetryCount(), 0u);
		EXO_ASSERT(!source.isExpired());
	return 1;
});

/* Found again - asked, or seen - ends the wait. */
EXO_TEST(sources_an_absent_user_found_again_is_asked_at_once,
{
		QuickDC::FileSource seen(HUB, "AAAA");
		seen.onAbsent();
		seen.touch();
		EXO_ASSERT(seen.isReadyToRetry());

		QuickDC::FileSource asked(HUB, "BBBB");
		asked.onAbsent();
		asked.onAttempt();
		EXO_ASSERT(asked.isReadyToRetry());
	return 1;
});

/* A peer not seen for a very long time has gone stale. */
EXO_TEST(sources_a_source_not_seen_for_years_has_gone_stale,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));

		file.getSource(0)->setLastSeen(Samurai::TimeStamp((time_t) 1000));
		EXO_ASSERT(file.getSource(0)->isExpired());
		EXO_ASSERT(file.getIdleSource() == 0);

		file.expireSources();
		EXO_ASSERT(file.countSources() == 0);
	
	return 1;
});

/* One being downloaded from is never expired out from under the transfer. */
EXO_TEST(sources_an_active_source_is_not_expired_mid_transfer,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));

		QuickDC::FileSource* source = file.getSource(0);
		source->setRetryCount(QUICKDC_SOURCE_MAX_RETRIES + 1);
		file.setSourceActive(source, true);

		file.expireSources();
		EXO_ASSERT(file.countSources() == 1);
	
	return 1;
});

/* Removing one takes it out of the active set too. */
EXO_TEST(sources_removing_a_source_clears_it_from_the_active_set,
{
		QuickDC::QueueFile file("/tmp/a.bin", TTH1, 1000);
		file.addSource(new QuickDC::FileSource(HUB, "AAAA"));

		QuickDC::FileSource* source = file.getSource(0);
		file.setSourceActive(source, true);
		file.removeSource(source);

		EXO_ASSERT(file.countSources() == 0);
		EXO_ASSERT(file.countActiveSources() == 0);
		EXO_ASSERT(file.getSource(0) == 0);
	
	return 1;
});

/* A url round trips through the source. */
EXO_TEST(sources_a_url_decodes_into_hub_and_client_id,
{
		QuickDC::FileSource source("adc://hub.example:412/AAAABBBB/TTH:" TTH1);
		EXO_ASSERT(source.isValid());
		EXO_ASSERT(source.getHubUrl() && !strcmp(source.getHubUrl(), "adc://hub.example:412"));
		EXO_ASSERT(source.getClientID() && !strcmp(source.getClientID(), "AAAABBBB"));
	
	return 1;
});

/* The point of the whole exercise: a queued TTH found in a peer's list. */
EXO_TEST(sources_a_queued_tth_in_a_peer_s_list_becomes_a_source,
{
		Fixture fix;
		fix.want("/tmp/wanted.bin", TTH1);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"theirname.bin\" Size=\"1000\" TTH=\"" TTH1 "\"/>"));

		EXO_ASSERT(added == 1);

		QuickDC::QueueFile* file = fix.queue->getFileByTTH(TTH1);
		EXO_ASSERT(file != 0);
		EXO_ASSERT(file && file->countSources() == 1);
		EXO_ASSERT(file && file->findSource(HUB, CID) != 0);
	
	return 1;
});

/* Matched on the TTH; what the peer calls it does not matter. */
EXO_TEST(sources_the_match_is_on_the_tth_not_the_name,
{
		Fixture fix;
		fix.want("/tmp/mine.bin", TTH1);

		fix.mineList(LIST(
			"<File Name=\"totally different name.bin\" Size=\"1000\" TTH=\"" TTH1 "\"/>"));

		QuickDC::QueueFile* file = fix.queue->getFileByTTH(TTH1);
		EXO_ASSERT(file && file->countSources() == 1);

		/* The peer's name is what we would have to ask for. */
		EXO_ASSERT(file && file->getSource(0)->getName()
			&& !strcmp(file->getSource(0)->getName(), "totally different name.bin"));
	
	return 1;
});

/* Nothing queued matches, so nothing is added. */
EXO_TEST(sources_a_list_with_nothing_we_want_adds_nothing,
{
		Fixture fix;
		fix.want("/tmp/wanted.bin", TTH1);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"other.bin\" Size=\"1000\" TTH=\"" TTH2 "\"/>"));

		EXO_ASSERT(added == 0);
		EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
	
	return 1;
});

/* Files nested in directories are reached. */
EXO_TEST(sources_the_walk_descends_into_directories,
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

		EXO_ASSERT(added == 2);
	
	return 1;
});

/* Several wanted files in one list. */
EXO_TEST(sources_several_wanted_files_are_all_picked_up,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);
		fix.want("/tmp/b.bin", TTH2);
		fix.want("/tmp/c.bin", TTH3);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>"
			"<File Name=\"x\" Size=\"1000\" TTH=\"XXXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A\"/>"
			"<File Name=\"b\" Size=\"1000\" TTH=\"" TTH2 "\"/>"));

		EXO_ASSERT(added == 2);
		EXO_ASSERT(fix.queue->getFileByTTH(TTH3)->countSources() == 0);
	
	return 1;
});

/* The same peer offered twice counts once. */
EXO_TEST(sources_a_source_we_already_had_is_not_counted_twice,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		EXO_ASSERT(fix.mineList(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>")) == 1);

		/* Asking the same peer again tells us nothing new. */
		EXO_ASSERT(fix.mineList(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>")) == 0);

		EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 1);
	
	return 1;
});

/* The same file listed twice by one peer is still one source. */
EXO_TEST(sources_one_peer_listing_a_file_twice_is_one_source,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"copy1\" Size=\"1000\" TTH=\"" TTH1 "\"/>"
			"<Directory Name=\"elsewhere\">"
			"<File Name=\"copy2\" Size=\"1000\" TTH=\"" TTH1 "\"/>"
			"</Directory>"));

		EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 1);
		EXO_ASSERT(added <= 2);
	
	return 1;
});

/* A bzip2 file list, which is what most peers actually send. */
EXO_TEST(sources_a_bzip2_file_list_is_unpacked_and_mined,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const std::string plain = LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>");
		const std::string packed = bzip2(plain);
		EXO_ASSERT(!packed.empty());
		EXO_ASSERT(packed.compare(0, 3, "BZh") == 0);

		EXO_ASSERT(fix.mineList(packed) == 1);
	
	return 1;
});

/* Entries with no TTH cannot be matched and must not crash the walk. */
EXO_TEST(sources_an_entry_without_a_tth_is_skipped,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const size_t added = fix.mineList(LIST(
			"<File Name=\"notth\" Size=\"1000\"/>"
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>"));

		EXO_ASSERT(added == 1);
	
	return 1;
});

/* Nothing usable in the file at all. */
EXO_TEST(sources_an_empty_or_unusable_list_adds_nothing,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		EXO_ASSERT(fix.mineList(LIST("")) == 0);
		EXO_ASSERT(fix.mineList("this is not a file list") == 0);
		EXO_ASSERT(fix.mineList("BZhthis is not bzip2 either") == 0);
	
	return 1;
});

/* A request whose download never arrived. */
EXO_TEST(sources_a_missing_download_adds_nothing,
{
	const std::string dir = scratch("lists");

		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		QuickDC::FilelistRequest request(HUB, CID, fix.dir.c_str());
		EXO_ASSERT(fix.queue->onFilelistDownloaded(&request, HUB, CID) == 0);
		EXO_ASSERT(fix.queue->onFilelistDownloaded(0, HUB, CID) == 0);
	
	return 1;
});

/* An empty queue has nothing to match against. */
EXO_TEST(sources_a_list_mined_against_an_empty_queue_adds_nothing,
{
		Fixture fix;
		EXO_ASSERT(fix.mineList(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>")) == 0);
	
	return 1;
});

/* An ADC CID and an ordinary nickname both still name a file in there. */
EXO_TEST(sources_a_cid_and_a_nickname_both_name_a_file_in_the,
{
	const std::string dir = scratch("lists");
	const std::string prefix = dir + "/";

		QuickDC::FilelistRequest byCid(HUB, CID, dir.c_str());
		EXO_ASSERT(byCid.isValid());
		EXO_ASSERT(byCid.getLocalFilename()
			&& byCid.getLocalFilename() == prefix + CID + ".xml");
		EXO_ASSERT(byCid.getTempFilename()
			&& byCid.getTempFilename() == prefix + CID + ".xml" QUICKDC_PART_SUFFIX);

		QuickDC::FilelistRequest byNick("dchub://hub.example:411", "[NL]alice", dir.c_str());
		EXO_ASSERT(byNick.isValid());
		EXO_ASSERT(byNick.getLocalFilename()
			&& byNick.getLocalFilename() == prefix + "[NL]alice.xml");
	
	return 1;
});

/* The queue refuses to hold one, so no download is ever started for it. */
EXO_TEST(sources_the_queue_refuses_a_request_it_cannot_name_a_file_for,
{
		Fixture fix;

		EXO_ASSERT(fix.queue->requestFilelist(HUB, "../../../../tmp/quickdc_traversal") == 0);
		EXO_ASSERT(fix.queue->requestFilelist(HUB, "a/b") == 0);
		EXO_ASSERT(fix.queue->countFilelistRequests() == 0);

		EXO_ASSERT(fix.queue->requestFilelist(HUB, CID) != 0);
		EXO_ASSERT(fix.queue->countFilelistRequests() == 1);
	
	return 1;
});

/*
	 * Far more than any share could account for, and refused on its size before
	 * anything unpacks it - so the perfectly good list at the front of it is
	 * never reached either.
	 */
EXO_TEST(sources_an_oversized_compressed_file_list_is_refused,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		std::string oversized = bzip2(LIST(
			"<File Name=\"a\" Size=\"1000\" TTH=\"" TTH1 "\"/>"));
		EXO_ASSERT(!oversized.empty());
		oversized.append((size_t) QUICKDC_FILELIST_MAX_PACKED, '\0');

		EXO_ASSERT(fix.mineList(oversized) == 0);
		EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
	
	return 1;
});

/* A bomb: a small stream that unpacks to more than the limit. */
EXO_TEST(sources_a_decompression_bomb_is_refused_and_leaves_nothing_behind,
{
	const std::string dir = scratch("lists");

		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const std::string block(1024 * 1024, '\0');
		const size_t blocks = (QUICKDC_FILELIST_MAX_PLAIN / (1024 * 1024)) + 16;

		const std::string bomb = bzip2_repeated(block, blocks);
		EXO_ASSERT(!bomb.empty());
		EXO_ASSERT(bomb.size() < 64 * 1024);   /* which is what makes it a bomb */

		QuickDC::FilelistRequest request(HUB, CID, fix.dir.c_str());
		EXO_ASSERT(write_file(request.getTempFilename(), bomb));

		EXO_ASSERT(fix.queue->onFilelistDownloaded(&request, HUB, CID) == 0);

		/*
		 * And nothing of it is left behind: a list that is short of entries but
		 * reads as complete would have us decide a peer holds nothing we want.
		 */
		EXO_ASSERT(!Samurai::IO::File(std::string(request.getLocalFilename())).exists());

		Samurai::IO::File(std::string(request.getTempFilename())).remove();
	
	return 1;
});

/*
	 * A stream that stops short. The blocks before the cut unpack perfectly well
	 * and the reader is forgiving about the tag that never closes, so what comes
	 * out reads as a complete list of a peer who holds less than they do - which
	 * is worse than no list at all, and so is refused.
	 */
EXO_TEST(sources_a_truncated_compressed_file_list_is_refused,
{
	const std::string dir = scratch("lists");

		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		const std::string packed = bzip2(big_list(40000));
		EXO_ASSERT(packed.size() > 1000);

		const std::string truncated = packed.substr(0, packed.size() - packed.size() / 8);

		QuickDC::FilelistRequest request(HUB, CID, fix.dir.c_str());
		EXO_ASSERT(write_file(request.getTempFilename(), truncated));

		EXO_ASSERT(fix.queue->onFilelistDownloaded(&request, HUB, CID) == 0);
		EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
		EXO_ASSERT(!Samurai::IO::File(std::string(request.getLocalFilename())).exists());

		Samurai::IO::File(std::string(request.getTempFilename())).remove();
	
	return 1;
});

/*
	 * Nesting. The tree is built and then walked recursively, and nothing in the
	 * format stops a peer nesting a directory inside itself until our stack runs
	 * out, so a document past the limit is refused rather than followed.
	 */
EXO_TEST(sources_ordinary_nesting_is_walked_to_the_bottom,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		/* Well within the limit: still walked to the bottom. */
		EXO_ASSERT(fix.mineList(nested_list(100)) == 1);
	
	return 1;
});

EXO_TEST(sources_a_file_list_nested_past_the_limit_is_refused,
{
		Fixture fix;
		fix.want("/tmp/a.bin", TTH1);

		EXO_ASSERT(fix.mineList(nested_list(QUICKDC_FILELIST_MAX_DEPTH + 64)) == 0);
		EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
	
	return 1;
});

/* ------------------------------------------------------------------------- */
/* How much of a file a source is asked for at a time                        */
/* ------------------------------------------------------------------------- */

/*
 * These were a run of checks sharing one Fixture in a single function. Split
 * one per property, each building its own, so a failure names the rule that
 * broke rather than the function that held all of them.
 */

EXO_TEST(sources_a_source_we_know_nothing_about_is_asked_for_the_default,
{
	Fixture fix;

	QuickDC::FileSource unknown(HUB, CID, "/tmp/a.bin");
	EXO_ASSERT(unknown.getThroughput() == 0);
	EXO_ASSERT(fix.queue->chunkSizeFor(&unknown) == QUICKDC_CHUNK_SIZE);
	EXO_ASSERT(fix.queue->chunkSizeFor(0) == QUICKDC_CHUNK_SIZE);
	return 1;
});

/* Slow: two seconds of it is less than the floor, so the floor stands. */
EXO_TEST(sources_a_slow_source_is_never_asked_for_less_than_the_default,
{
	Fixture fix;

	QuickDC::FileSource slow(HUB, CID, "/tmp/b.bin");
	slow.onChunkDone(64 * 1024, 1.0);
	EXO_ASSERT(slow.getThroughput() == 64 * 1024);
	EXO_ASSERT(fix.queue->chunkSizeFor(&slow) == QUICKDC_CHUNK_SIZE);
	return 1;
});

/* Quick: about QUICKDC_CHUNK_TARGET_SECONDS of transfer. */
EXO_TEST(sources_a_quick_source_is_asked_for_a_couple_of_seconds_worth,
{
	Fixture fix;

	QuickDC::FileSource quick(HUB, CID, "/tmp/c.bin");
	quick.onChunkDone(4 * 1024 * 1024, 1.0);
	EXO_ASSERT(quick.getThroughput() == 4u * 1024 * 1024);
	EXO_ASSERT(fix.queue->chunkSizeFor(&quick)
		== 4u * 1024 * 1024 * QUICKDC_CHUNK_TARGET_SECONDS);
	return 1;
});

/*
 * And a very fast one is held at the ceiling. Twenty five megabytes a second is
 * where a megabyte chunk was being asked for twenty five times over.
 */
EXO_TEST(sources_however_fast_the_source_a_chunk_stops_at_the_ceiling,
{
	Fixture fix;

	QuickDC::FileSource fast(HUB, CID, "/tmp/d.bin");
	for (int n = 0; n < 20; n++) fast.onChunkDone(25 * 1024 * 1024, 1.0);
	EXO_ASSERT(fast.getThroughput() > 20u * 1024 * 1024);
	EXO_ASSERT(fix.queue->chunkSizeFor(&fast) == (uint64_t) QUICKDC_CHUNK_SIZE_MAX);
	return 1;
});

/* Smoothed, so one slow chunk does not collapse the next request. */
EXO_TEST(sources_one_slow_chunk_does_not_undo_what_a_source_has_shown,
{
	QuickDC::FileSource fast(HUB, CID, "/tmp/d.bin");
	for (int n = 0; n < 20; n++) fast.onChunkDone(25 * 1024 * 1024, 1.0);

	const uint64_t settled = fast.getThroughput();
	fast.onChunkDone(1024, 1.0);
	EXO_ASSERT(fast.getThroughput() > settled / 2);
	return 1;
});

/* Nothing arriving, or arriving in no time at all, measures nothing. */
EXO_TEST(sources_a_chunk_of_nothing_or_in_no_time_is_not_a_measurement,
{
	QuickDC::FileSource odd(HUB, CID, "/tmp/e.bin");
	odd.onChunkDone(0, 1.0);
	odd.onChunkDone(1024, 0.0);
	odd.onChunkDone(1024, -1.0);
	EXO_ASSERT(odd.getThroughput() == 0);
	return 1;
});

/*
 * None of these may become a path. Was written at function level, sharing the
 * scratch directory with the case above it.
 */
EXO_TEST(sources_an_identifier_that_would_name_a_path_is_refused,
{
	const std::string dir = scratch("lists");
	const std::string prefix = dir + "/";

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

		EXO_ASSERT(!request.isValid());
		EXO_ASSERT(request.getLocalFilename() == 0);
		EXO_ASSERT(request.getTempFilename() == 0);

		/*
		 * And if one were ever accepted, what it names still has to sit inside
		 * the directory it was given - which is the property that matters
		 * whatever the identifier turns out to contain.
		 */
		if (request.getLocalFilename())
		{
			const std::string resolved =
				Samurai::IO::File::resolvePath(request.getLocalFilename());
			EXO_ASSERT(resolved.compare(0, prefix.size(), prefix) == 0);
		}
	}
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Sources for a file somebody is waiting to read                            */
/* ------------------------------------------------------------------------- */

/*
 * The rotation searches for one file per QUICKDC_SOURCE_SEARCH_INTERVAL, which
 * is right for a queue nobody is watching and hopeless for a document somebody
 * has just opened: a bulletin board thread of ten replies would be twenty
 * minutes before the last of the bodies was even looked for.
 * requestSourceSearch() is that search asked for now, bounded so that it cannot
 * become the flood a hub disconnects clients for; addKnownSource() is the case
 * where no search is needed at all, because the post's index entry named the CID
 * the hub accepted it from.
 *
 * There is no Core in these cases and nothing can leave without one, which is
 * what SourceSearchNotSent reports - and it is reported after the burst has been
 * charged for the asking. So what is checked here is which answer comes back and
 * what the queue looks like afterwards; whether a search reaches a hub is the
 * business of the hub session cases.
 */

/* The author of a post is a source for it, with no search anywhere. */
EXO_TEST(sources_a_named_source_is_added_without_any_search,
{
	Fixture fix;
	fix.want("/tmp/post.txt", TTH1);

	EXO_ASSERT(fix.queue->addKnownSource(TTH1, HUB, CID));

	QuickDC::QueueFile* file = fix.queue->getFileByTTH(TTH1);
	EXO_ASSERT(file && file->countSources() == 1);

	QuickDC::FileSource* source = file ? file->findSource(HUB, CID) : 0;
	EXO_ASSERT(source != 0);

	/*
	 * Named by the hash rather than left nameless: a source with no filename at
	 * all is a request for the peer's file list. What goes on the wire is
	 * "TTH/{root}" either way, built from the QueueFile and not from this.
	 */
	EXO_ASSERT(source && !source->isFilelist());
	EXO_ASSERT(source && source->getName() && !strcmp(source->getName(), "TTH:" TTH1));
	return 1;
});

/* And it is still the same peer after a trip through the queue file notation. */
EXO_TEST(sources_a_named_source_survives_the_queue_file_notation,
{
	Fixture fix;
	fix.want("/tmp/post.txt", TTH1);
	EXO_ASSERT(fix.queue->addKnownSource(TTH1, HUB, CID));

	QuickDC::FileSource* source = fix.queue->getFileByTTH(TTH1)->findSource(HUB, CID);
	EXO_ASSERT(source != 0);

	char* url = source->toURL();
	EXO_ASSERT(url && std::string(url) == HUB "/" CID "/TTH:" TTH1);

	QuickDC::FileSource back(url);
	free(url);

	EXO_ASSERT(back.isValid());
	EXO_ASSERT(!back.isFilelist());
	EXO_ASSERT(back.matches(HUB, CID));
	return 1;
});

/* Which is enough for a transfer to be started from them. */
EXO_TEST(sources_a_named_source_is_one_a_transfer_can_start_from,
{
	Fixture fix;
	fix.want("/tmp/post.txt", TTH1);
	EXO_ASSERT(fix.queue->addKnownSource(TTH1, HUB, CID));

	QuickDC::QueueFile* file = 0;
	QuickDC::FileSource* source = 0;

	EXO_ASSERT(fix.queue->getNextTransfer(file, source));
	EXO_ASSERT(file == fix.queue->getFileByTTH(TTH1));
	EXO_ASSERT(source && source->matches(HUB, CID));
	return 1;
});

/* The same peer named twice is one source, and the second time says so. */
EXO_TEST(sources_the_same_named_source_twice_is_refused,
{
	Fixture fix;
	fix.want("/tmp/post.txt", TTH1);

	EXO_ASSERT(fix.queue->addKnownSource(TTH1, HUB, CID));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, HUB, CID));
	EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 1);

	/* Somebody else on the same hub is another matter. */
	EXO_ASSERT(fix.queue->addKnownSource(TTH1, HUB, "SOMEONEELSEAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
	EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 2);
	return 1;
});

/* Neither of them has anything to say about a hash we are not downloading. */
EXO_TEST(sources_a_hash_that_is_not_queued_is_refused_by_both,
{
	Fixture fix;
	fix.want("/tmp/post.txt", TTH1);

	EXO_ASSERT(!fix.queue->addKnownSource(TTH2, HUB, CID));

	QuickDC::QueueManager::SourceSearchResult why = QuickDC::QueueManager::SourceSearchSent;
	EXO_ASSERT(!fix.queue->requestSourceSearch(TTH2, &why));
	EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchNotQueued);

	/* Refusing it costs nothing, so the burst is intact for one we do want. */
	EXO_ASSERT(!fix.queue->requestSourceSearch(TTH1, &why));
	EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchNotSent);
	return 1;
});

/*
 * A hash that is not a root goes no further. The length alone would not do it:
 * these go on the wire in a search, and a hub is no place to discover that
 * something was never base32.
 */
EXO_TEST(sources_a_malformed_hash_is_refused_by_both,
{
	Fixture fix;
	fix.want("/tmp/post.txt", TTH1);

	static const char* malformed[] = {
		"",
		"TTH:" TTH1,
		"MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7",     /* one short */
		"MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7AA",   /* one long */
		"mwxflx5no4t54fmvtgz6uxer2qkn6j2iggl3w7a",    /* not upper case */
		"MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W71",    /* '1' is not base32 */
		"MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7 ",
		0
	};

	for (size_t n = 0; malformed[n]; n++)
	{
		QuickDC::QueueManager::SourceSearchResult why = QuickDC::QueueManager::SourceSearchSent;

		EXO_ASSERT(!fix.queue->addKnownSource(malformed[n], HUB, CID));
		EXO_ASSERT(!fix.queue->requestSourceSearch(malformed[n], &why));
		EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchBadHash);
	}

	QuickDC::QueueManager::SourceSearchResult why = QuickDC::QueueManager::SourceSearchSent;
	EXO_ASSERT(!fix.queue->addKnownSource(0, HUB, CID));
	EXO_ASSERT(!fix.queue->requestSourceSearch(0, &why));
	EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchBadHash);

	EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
	return 1;
});

/*
 * And a peer the queue could not write back out again is no source. One is
 * spelled {hub}/{cid}/TTH:{root} and read back by splitting on the slashes, so
 * an identifier carrying one of those would return as somebody else; an empty
 * hub or client ID is worse still, since two of those match each other and the
 * next source added would be taken for one we already had.
 */
EXO_TEST(sources_a_peer_the_queue_could_not_write_back_is_refused,
{
	Fixture fix;
	fix.want("/tmp/post.txt", TTH1);

	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, 0, CID));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, "", CID));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, "hub.example:412", CID));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, "://hub.example:412", CID));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, "adc://", CID));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, HUB "/somewhere", CID));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, HUB "/", CID));

	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, HUB, 0));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, HUB, ""));
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, HUB, "AAAA/BBBB"));

	EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 0);
	return 1;
});

/*
 * The burst is what stops a caller in a loop from becoming a search flood. It is
 * charged for the asking and not for the sending, which is why these get as far
 * as having nowhere to send them and then stop.
 */
EXO_TEST(sources_the_burst_bounds_how_often_a_search_may_be_asked_for,
{
	Fixture fix;
	fix.want("/tmp/post.txt", TTH1);

	QuickDC::QueueManager::SourceSearchResult why = QuickDC::QueueManager::SourceSearchSent;

	for (size_t n = 0; n < QUICKDC_SOURCE_SEARCH_BURST; n++)
	{
		EXO_ASSERT(!fix.queue->requestSourceSearch(TTH1, &why));
		EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchNotSent);
	}

	EXO_ASSERT(!fix.queue->requestSourceSearch(TTH1, &why));
	EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchRateLimited);

	/* And it stays refused; the next credit is a whole interval away. */
	EXO_ASSERT(!fix.queue->requestSourceSearch(TTH1, &why));
	EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchRateLimited);

	/* The reason is optional, for a caller that only wants to know whether. */
	EXO_ASSERT(!fix.queue->requestSourceSearch(TTH1));
	return 1;
});

/*
 * The regression that would matter most: none of that may cost the rest of the
 * queue its rotation. A fresh queue has never searched, so its turn is due at
 * once, and it is still due once a burst has been spent.
 */
EXO_TEST(sources_asking_for_a_search_now_leaves_the_rotation_alone,
{
	Fixture fix;
	fix.want("/tmp/a.bin", TTH1);
	fix.want("/tmp/b.bin", TTH2);

	EXO_ASSERT(fix.queue->rotation() == 0);
	EXO_ASSERT(fix.queue->sinceRotation() >= QUICKDC_SOURCE_SEARCH_INTERVAL);

	for (size_t n = 0; n < QUICKDC_SOURCE_SEARCH_BURST + 2; n++)
		fix.queue->requestSourceSearch(TTH1);

	EXO_ASSERT(fix.queue->addKnownSource(TTH1, HUB, CID));

	EXO_ASSERT(fix.queue->rotation() == 0);
	EXO_ASSERT(fix.queue->sinceRotation() >= QUICKDC_SOURCE_SEARCH_INTERVAL);

	/* It still runs, and with nowhere to send anything it keeps its turn rather
	   than spending the interval on a search that never left. */
	fix.queue->rotate();
	EXO_ASSERT(fix.queue->rotation() == 0);
	EXO_ASSERT(fix.queue->sinceRotation() >= QUICKDC_SOURCE_SEARCH_INTERVAL);

	/* And both files are still there, still short of sources, still wanted. */
	std::vector<QuickDC::QueueFile*> wanted;
	fix.queue->getFilesNeedingSources(wanted, QUICKDC_SOURCE_SEARCH_MIN_SOURCES);

	EXO_ASSERT(wanted.size() == 2);
	EXO_ASSERT(fix.queue->getFileByTTH(TTH1)->countSources() == 1);
	EXO_ASSERT(fix.queue->getFileByTTH(TTH2)->countSources() == 0);
	return 1;
});

/* Neither minds being asked before anything is queued at all. */
EXO_TEST(sources_an_empty_queue_refuses_both_without_complaint,
{
	Fixture fix;

	QuickDC::QueueManager::SourceSearchResult why = QuickDC::QueueManager::SourceSearchSent;

	EXO_ASSERT(!fix.queue->requestSourceSearch(TTH1, &why));
	EXO_ASSERT(why == QuickDC::QueueManager::SourceSearchNotQueued);
	EXO_ASSERT(!fix.queue->addKnownSource(TTH1, HUB, CID));
	return 1;
});
