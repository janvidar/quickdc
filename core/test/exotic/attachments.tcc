/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Offering a file by its hash alone: what attach() makes reachable, and what
 * detach() takes back again.
 *
 * The hash is the whole of the capability, so the two halves of that are worth
 * as much as each other. A file that turned up in a file list or answered a
 * search by name would be shared, which is not what the user asked for. A hash
 * that goes on being answered after the file has been withdrawn promises a peer
 * something we will not serve, and it is a search hit that leads to a failed
 * transfer rather than to nothing at all.
 *
 * Run against a real share, a real hash manager and a real configuration file,
 * because every one of those is part of the answer: the tables are only filled
 * as each TTH arrives, the file list is written to disk, and what is remembered
 * for the next time the client starts is a group in the ini file.
 */

#include "quickdc.h"
#include "api/attachments.h"
#include "api/core.h"
#include "config/preferences.h"
#include "hash/hashmanager.h"
#include "share/sharemanager.h"
#include "share/sharefile.h"
#include "share/searchrequest.h"

#include <samurai/io/file.h>

#include <chrono>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

using Persistence = QuickDC::Share::Manager::Persistence;
using Request = QuickDC::Share::SearchRequest;

/* The pid is in the name because a run that dies leaves its files behind, and
   the next one must not find them and take them for its own. */
static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_attach_" + std::to_string((int) getpid())
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

/* What the share manager said about the files handed to attach(). */
class Attacher : public QuickDC::Share::AttachmentListener
{
	public:
		void EventAttachmentReady(const char* path, const char* tth, uint64_t size) override
		{
			ready++;
			lastPath = path ? path : "";
			lastTTH = tth ? tth : "";
			lastSize = size;
		}

		void EventAttachmentFailed(const char* path, const char* reason) override
		{
			failed++;
			lastPath = path ? path : "";
			lastReason = reason ? reason : "";
		}

		int ready = 0;
		int failed = 0;
		std::string lastPath;
		std::string lastTTH;
		std::string lastReason;
		uint64_t lastSize = 0;
};

/* Collects what a search matched, so a hit can be told from silence. */
class Replies : public QuickDC::Share::SearchReplyHandler
{
	public:
		void EventSearchReply(QuickDC::Share::SearchReply* reply) override
		{
			if (reply && reply->getFile())
				names.push_back(reply->getFile()->getPublicName());
		}

		size_t count() const { return names.size(); }

		std::vector<std::string> names;
};

/*
 * A share directory, a core, a hash manager of its own, and the share manager
 * under test. Split from the scan, because a couple of the cases below have to
 * attach a file before the share has been looked at.
 */
class Fixture
{
	public:
		std::string dir;      /* the shared directory */
		std::string out;      /* where the file lists are written */
		std::string cfg;
		std::string ses;
		std::string hashcache;
		std::vector<std::string> madeFiles;
		std::unique_ptr<QuickDC::Core> core;
		std::unique_ptr<QuickDC::Hash::Manager> hashes;
		QuickDC::Share::Manager* shares = 0;

		Fixture()
			: dir(scratch("share")), out(scratch("out")),
			  cfg(scratch("config.conf")), ses(scratch("session.conf")),
			  hashcache(scratch("hashdb.cache"))
		{
			Samurai::IO::File::mkdir(dir.c_str());
			Samurai::IO::File::mkdir(out.c_str());
		}

		~Fixture()
		{
			/* The share files hold jobs in the hash manager, so they go first. */
			shares = 0;
			core.reset();
			hashes.reset();

			Samurai::IO::File(hashcache).remove();
			Samurai::IO::File(hashcache + ".new").remove();

			for (size_t n = 0; n < madeFiles.size(); n++)
				Samurai::IO::File(madeFiles[n]).remove();

			Samurai::IO::File(out + "/files.xml").remove();
			Samurai::IO::File(out + "/files.xml.bz2").remove();

			/* rmdir, since remove() unlinks and never takes a directory away. */
			Samurai::IO::File::rmdir(out.c_str());
			Samurai::IO::File::rmdir(dir.c_str());
			Samurai::IO::File(cfg).remove();
			Samurai::IO::File(ses).remove();
		}

		Fixture(const Fixture&) = delete;
		Fixture& operator=(const Fixture&) = delete;

		/* A file in the shared directory, which is published like any other. */
		std::string sharedFile(const char* leaf, const std::string& content)
		{
			const std::string path = dir + "/" + leaf;
			writeFile(path, content);
			madeFiles.push_back(path);
			return path;
		}

		/* And one outside it, which is nobody's business until it is attached. */
		std::string looseFile(const char* leaf, const std::string& content)
		{
			const std::string path = scratch(leaf);
			writeFile(path, content);
			madeFiles.push_back(path);
			return path;
		}

		void build(bool withShare)
		{
			writeFile(cfg,
				"[User]\nNickname=tester\n\n"
				"[Shares]\nTotal shares=" + std::string(withShare ? "1" : "0") + "\n"
				"Share 0=shared:" + dir + "\n");

			core = std::make_unique<QuickDC::Core>(cfg, ses);

			/*
			 * A share file registers a hashing job as it is found, and an
			 * attachment is hashed the same way, so there has to be a manager to
			 * register with - pointed at a cache of its own rather than the real
			 * database.
			 */
			hashes = std::make_unique<QuickDC::Hash::Manager>(hashcache.c_str());
			shares = new QuickDC::Share::Manager(core->config, hashes.get(), out.c_str());

			/* The core owns whatever it built; this one replaces it. */
			delete core->shares;
			core->shares = shares;
		}

		/* Scans the share, and hashes what that found. */
		bool scan()
		{
			shares->initialize();
			return drain();
		}

		/*
		 * Runs the manager until the worker has nothing left, as the main loop
		 * does. Nothing about an attachment is known until then: the hash arrives
		 * on this thread, from process().
		 */
		bool drain()
		{
			const auto deadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(120);
			while (hashes->process() || hashes->isBusy())
			{
				if (std::chrono::steady_clock::now() > deadline) return false;
				usleep(1000);
			}
			hashes->process();
			return true;
		}

		std::string list() const { return readFile(out + "/files.xml"); }

		/* What the configuration will offer again at the next start-up. */
		int remembered() const
		{
			return core->config->peekNumber("Attachments", "Total", 0);
		}

		std::string rememberedEntry(int n) const
		{
			char key[16];
			snprintf(key, sizeof(key), "File %d", n);
			return core->config->peekString("Attachments", key, "");
		}

		size_t hitsFor(const char* term)
		{
			Replies replies;
			Request req(&replies);
			req.addInclusion(term);
			shares->search(&req);
			return replies.count();
		}

		size_t hitsForTTH(const std::string& tth)
		{
			Replies replies;
			Request req(&replies);
			req.setTTH(tth.c_str());
			shares->search(&req);
			return replies.count();
		}
};

/* A TTH root is 39 base32 characters. */
#define TTH_LENGTH 39

/* The hash comes back, and the file is reachable by it and by nothing else. */
EXO_TEST(attachments_an_attached_file_is_reachable_by_its_hash,
{
		Fixture f;
		f.build(false);
		EXO_ASSERT(f.scan());

		const std::string path = f.looseFile("note.txt", std::string(40000, 'a'));

		Attacher who;
		EXO_ASSERT(f.shares->attach(path.c_str(), &who));
		EXO_ASSERT(f.drain());

		EXO_ASSERT(who.ready == 1);
		EXO_ASSERT(who.failed == 0);
		EXO_ASSERT(who.lastPath == path);
		EXO_ASSERT(who.lastTTH.size() == TTH_LENGTH);
		EXO_ASSERT(who.lastSize == 40000);

		Samurai::IO::File* served = f.shares->getFileByTTH(who.lastTTH.c_str());
		EXO_ASSERT(served);
		EXO_ASSERT(f.hitsForTTH(who.lastTTH) == 1);

	return 1;
});

/* Nothing that is offered by hash is offered by name. */
EXO_TEST(attachments_an_attached_file_is_in_no_file_list_and_answers_no_name,
{
		Fixture f;
		f.sharedFile("public.bin", std::string(1000, 'p'));
		f.build(true);
		EXO_ASSERT(f.scan());

		const uint32_t filesBefore = f.shares->getFiles();
		const uint64_t sizeBefore = f.shares->getSharedSize();

		const std::string path = f.looseFile("private.bin", std::string(2000, 'q'));

		Attacher who;
		EXO_ASSERT(f.shares->attach(path.c_str(), &who));
		EXO_ASSERT(f.drain());
		EXO_ASSERT(who.ready == 1);

		f.shares->createFileLists(false);
		const std::string xml = f.list();

		EXO_ASSERT(xml.find("public.bin") != std::string::npos);
		EXO_ASSERT(xml.find("private.bin") == std::string::npos);
		EXO_ASSERT(xml.find(who.lastTTH) == std::string::npos);

		EXO_ASSERT(f.hitsFor("public") == 1);
		EXO_ASSERT(f.hitsFor("private") == 0);

		/* And a hub is told the same numbers as before. */
		EXO_ASSERT(f.shares->getFiles() == filesBefore);
		EXO_ASSERT(f.shares->getSharedSize() == sizeBefore);

	return 1;
});

/* Taking it back stops it being answered for. */
EXO_TEST(attachments_a_detached_file_is_no_longer_offered,
{
		Fixture f;
		f.build(false);
		EXO_ASSERT(f.scan());

		const std::string path = f.looseFile("gone.txt", std::string(30000, 'b'));

		Attacher who;
		EXO_ASSERT(f.shares->attach(path.c_str(), &who));
		EXO_ASSERT(f.drain());
		EXO_ASSERT(who.ready == 1);
		EXO_ASSERT(f.shares->getFileByTTH(who.lastTTH.c_str()));

		EXO_ASSERT(f.shares->detach(who.lastTTH));

		EXO_ASSERT(!f.shares->getFileByTTH(who.lastTTH.c_str()));
		EXO_ASSERT(f.hitsForTTH(who.lastTTH) == 0);

		/* And a second time says there was nothing left to take back. */
		EXO_ASSERT(!f.shares->detach(who.lastTTH));

	return 1;
});

/* A hash nobody offered is not an error, and neither is no hash at all. */
EXO_TEST(attachments_detaching_a_hash_that_was_never_attached_says_so,
{
		Fixture f;
		f.build(false);
		EXO_ASSERT(f.scan());

		EXO_ASSERT(!f.shares->detach("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFG"));
		EXO_ASSERT(!f.shares->detach(""));

	return 1;
});

/*
	 * The same content shared and attached at once. The share is what makes it
	 * reachable there, so taking the attachment back may not stop it being
	 * served: it is in a file list, it answers a search by name, and a peer that
	 * found it that way asks for it by hash.
	 */
EXO_TEST(attachments_detaching_leaves_a_file_that_is_genuinely_shared_alone,
{
		Fixture f;
		const std::string content(50000, 'c');
		f.sharedFile("published.bin", content);
		f.build(true);
		EXO_ASSERT(f.scan());

		/* A copy, so the two are different files with one hash between them. */
		const std::string copy = f.looseFile("copy.bin", content);

		Attacher who;
		EXO_ASSERT(f.shares->attach(copy.c_str(), &who));
		EXO_ASSERT(f.drain());
		EXO_ASSERT(who.ready == 1);
		EXO_ASSERT(f.shares->getFileByTTH(who.lastTTH.c_str()));

		EXO_ASSERT(f.shares->detach(who.lastTTH));

		Samurai::IO::File* served = f.shares->getFileByTTH(who.lastTTH.c_str());
		EXO_ASSERT(served);
		EXO_ASSERT(served->getName().find("published.bin") != std::string::npos);
		EXO_ASSERT(f.hitsForTTH(who.lastTTH) == 1);
		EXO_ASSERT(f.hitsFor("published") == 1);

	return 1;
});

/*
	 * And the other way round, which is a matter of timing rather than of what
	 * the user did: a share is hashed in the background, so an attachment can
	 * have claimed the hash before the shared file that carries it was hashed.
	 * The shared file's own registration finds the entry taken and does nothing,
	 * which leaves the attachment holding the only way to ask for either of them.
	 */
EXO_TEST(attachments_detaching_hands_the_hash_back_to_a_shared_copy,
{
		Fixture f;
		const std::string content(60000, 'd');
		f.sharedFile("published.bin", content);
		f.build(true);

		const std::string copy = f.looseFile("copy.bin", content);

		Attacher who;
		EXO_ASSERT(f.shares->attach(copy.c_str(), &who, Persistence::Session));
		EXO_ASSERT(f.drain());
		EXO_ASSERT(who.ready == 1);

		/* Only now is the share looked at, so the attachment got there first. */
		EXO_ASSERT(f.scan());
		Samurai::IO::File* attached = f.shares->getFileByTTH(who.lastTTH.c_str());
		EXO_ASSERT(attached);
		EXO_ASSERT(attached->getName().find("copy.bin") != std::string::npos);

		EXO_ASSERT(f.shares->detach(who.lastTTH));

		Samurai::IO::File* served = f.shares->getFileByTTH(who.lastTTH.c_str());
		EXO_ASSERT(served);
		EXO_ASSERT(served->getName().find("published.bin") != std::string::npos);
		EXO_ASSERT(f.hitsForTTH(who.lastTTH) == 1);

	return 1;
});

/* A chat attachment is remembered, so an old message goes on working. */
EXO_TEST(attachments_a_remembered_attachment_reaches_the_configuration,
{
		Fixture f;
		f.build(false);
		EXO_ASSERT(f.scan());

		const std::string path = f.looseFile("kept.txt", std::string(10000, 'e'));

		Attacher who;
		EXO_ASSERT(f.shares->attach(path.c_str(), &who, Persistence::Remembered));
		EXO_ASSERT(f.drain());
		EXO_ASSERT(who.ready == 1);

		EXO_ASSERT(f.remembered() == 1);

		const std::string entry = f.rememberedEntry(0);
		EXO_ASSERT(entry.compare(0, TTH_LENGTH, who.lastTTH) == 0);
		EXO_ASSERT(entry[TTH_LENGTH] == ' ');
		EXO_ASSERT(entry.find("kept.txt") != std::string::npos);

	return 1;
});

/*
	 * A session attachment is not. Whoever attached it keeps its own record of
	 * what it holds and decides for itself what to offer again, and an entry here
	 * as well would be offered whatever that caller decided.
	 */
EXO_TEST(attachments_a_session_attachment_stays_out_of_the_configuration,
{
		Fixture f;
		f.build(false);
		EXO_ASSERT(f.scan());

		const std::string path = f.looseFile("passing.txt", std::string(10000, 'f'));

		Attacher who;
		EXO_ASSERT(f.shares->attach(path.c_str(), &who, Persistence::Session));
		EXO_ASSERT(f.drain());

		/* Offered exactly as well as any other, all the same. */
		EXO_ASSERT(who.ready == 1);
		EXO_ASSERT(f.shares->getFileByTTH(who.lastTTH.c_str()));
		EXO_ASSERT(f.hitsForTTH(who.lastTTH) == 1);

		EXO_ASSERT(f.remembered() == 0);
		EXO_ASSERT(f.rememberedEntry(0).empty());

	return 1;
});

/* Asking for one of those to be remembered after all writes it down. */
EXO_TEST(attachments_a_session_attachment_is_promoted_when_it_is_attached_again,
{
		Fixture f;
		f.build(false);
		EXO_ASSERT(f.scan());

		const std::string path = f.looseFile("twice.txt", std::string(10000, 'g'));

		Attacher first;
		EXO_ASSERT(f.shares->attach(path.c_str(), &first, Persistence::Session));
		EXO_ASSERT(f.drain());
		EXO_ASSERT(first.ready == 1);
		EXO_ASSERT(f.remembered() == 0);

		/* The hash is known by now, so this one is answered on the spot. */
		Attacher again;
		EXO_ASSERT(f.shares->attach(path.c_str(), &again, Persistence::Remembered));
		EXO_ASSERT(again.ready == 1);
		EXO_ASSERT(again.lastTTH == first.lastTTH);

		EXO_ASSERT(f.remembered() == 1);
		EXO_ASSERT(f.rememberedEntry(0).find("twice.txt") != std::string::npos);

		/* And not the other way about: what was promised is not taken away. */
		Attacher third;
		EXO_ASSERT(f.shares->attach(path.c_str(), &third, Persistence::Session));
		EXO_ASSERT(third.ready == 1);
		EXO_ASSERT(f.remembered() == 1);

	return 1;
});

/* Taking one back takes it out of the configuration as well. */
EXO_TEST(attachments_a_detached_file_is_not_remembered_for_the_next_start,
{
		Fixture f;
		f.build(false);
		EXO_ASSERT(f.scan());

		const std::string one = f.looseFile("one.txt", std::string(10000, 'h'));
		const std::string two = f.looseFile("two.txt", std::string(11000, 'i'));

		Attacher a, b;
		EXO_ASSERT(f.shares->attach(one.c_str(), &a));
		EXO_ASSERT(f.shares->attach(two.c_str(), &b));
		EXO_ASSERT(f.drain());
		EXO_ASSERT(a.ready == 1 && b.ready == 1);
		EXO_ASSERT(f.remembered() == 2);

		EXO_ASSERT(f.shares->detach(a.lastTTH));

		EXO_ASSERT(f.remembered() == 1);
		EXO_ASSERT(f.rememberedEntry(0).find("two.txt") != std::string::npos);
		EXO_ASSERT(f.rememberedEntry(1).empty());

		/* What is left is still offered. */
		EXO_ASSERT(f.shares->getFileByTTH(b.lastTTH.c_str()));

	return 1;
});

/*
	 * The failure path. A hash job that cannot be run is only written to the log,
	 * so the file is recognised as never getting a hash when hashing next falls
	 * idle - and the record has to be gone by then, with nothing of it left
	 * behind in the tables or the configuration.
	 *
	 * The big file is what makes that deterministic: there is one worker thread
	 * taking jobs in turn, so removing the second file while the first is being
	 * hashed happens before anything has tried to open it.
	 */
EXO_TEST(attachments_a_file_that_cannot_be_hashed_is_reported_and_forgotten,
{
		Fixture f;
		f.build(false);
		EXO_ASSERT(f.scan());

		const std::string big = f.looseFile("big.bin", std::string(8 * 1024 * 1024, 'j'));
		const std::string doomed = f.looseFile("doomed.bin", std::string(20000, 'k'));

		Attacher keep, lost;
		EXO_ASSERT(f.shares->attach(big.c_str(), &keep));
		EXO_ASSERT(f.shares->attach(doomed.c_str(), &lost));

		Samurai::IO::File(doomed).remove();

		EXO_ASSERT(f.drain());

		EXO_ASSERT(keep.ready == 1);
		EXO_ASSERT(lost.ready == 0);
		EXO_ASSERT(lost.failed == 1);
		EXO_ASSERT(lost.lastPath == doomed);
		EXO_ASSERT(!lost.lastReason.empty());

		/* Only the one that worked is remembered, and only it is offered. */
		EXO_ASSERT(f.remembered() == 1);
		EXO_ASSERT(f.rememberedEntry(0).find("big.bin") != std::string::npos);
		EXO_ASSERT(f.shares->getFileByTTH(keep.lastTTH.c_str()));

		/* And the record is gone, so the file can be attached again as new. */
		EXO_ASSERT(writeFile(doomed, std::string(20000, 'k')));

		Attacher retry;
		EXO_ASSERT(f.shares->attach(doomed.c_str(), &retry));
		EXO_ASSERT(f.drain());
		EXO_ASSERT(retry.ready == 1);
		EXO_ASSERT(retry.lastTTH.size() == TTH_LENGTH);
		EXO_ASSERT(f.shares->getFileByTTH(retry.lastTTH.c_str()));
		EXO_ASSERT(f.remembered() == 2);

	return 1;
});
