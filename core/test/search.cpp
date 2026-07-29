/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * A search arrives from a hub, is turned into a SearchRequest, and is run
 * against our own share. What comes back names our files to whoever asked, so
 * the matching rules are what decides both whether we are findable at all and
 * what we hand out - a request asking for one thing and being answered with
 * another is not a cosmetic fault.
 *
 * The matching runs against a real scanned and hashed share, because the tables
 * it searches are only filled as each file's TTH arrives.
 */

#include "quickdc.h"
#include "api/core.h"
#include "config/preferences.h"
#include "hash/hashmanager.h"
#include "share/sharemanager.h"
#include "share/sharefile.h"
#include "share/searchmanager.h"
#include "share/searchrequest.h"
#include "share/searchresult.h"

#include <samurai/io/file.h>

#include <chrono>
#include <memory>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

static int failures = 0;

/* Counting allocations is how the search manager's bookkeeping is checked. */
static size_t live = 0;

void* operator new(size_t n)
{
	void* p = malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	live++;
	return p;
}

void operator delete(void* p) noexcept
{
	if (!p) return;
	live--;
	free(p);
}

void* operator new[](size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

using Request = QuickDC::Share::SearchRequest;

static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_search_" + std::to_string((int) getpid())
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

/* Collects what a search matched, in the order it was reported. */
class Replies : public QuickDC::Share::SearchReplyHandler
{
	public:
		void EventSearchReply(QuickDC::Share::SearchReply* reply)
		{
			if (reply && reply->getFile())
			{
				names.push_back(reply->getFile()->getPublicName());
				files.push_back(reply->getFile());
			}
		}

		bool has(const char* name) const
		{
			for (size_t n = 0; n < names.size(); n++)
				if (names[n].find(name) != std::string::npos) return true;
			return false;
		}

		std::string joined() const
		{
			std::string out;
			for (size_t n = 0; n < names.size(); n++)
			{
				if (n) out += " ";
				out += names[n];
			}
			return out;
		}

		size_t count() const { return names.size(); }

		std::vector<std::string> names;
		std::vector<QuickDC::Share::File*> files;
};

/* Takes whatever the search manager routes to it. */
class Results : public QuickDC::Share::SearchResultHandler
{
	public:
		Results() : received(0) { }
		void EventSearchResult(QuickDC::Share::SearchResult*) { received++; }
		int received;
};

/*
 * A directory of files, scanned into a share manager with its own hash cache,
 * so a search has something real to run against.
 */
class Share
{
	public:
		std::string dir;
		std::string out;
		std::string cfg;
		std::string ses;
		std::string hashcache;
		std::vector<std::string> madeFiles;
		std::unique_ptr<QuickDC::Core> core;
		std::unique_ptr<QuickDC::Hash::Manager> hashes;
		QuickDC::Share::Manager* shares = 0;

		Share()
			: dir(scratch("share")), out(scratch("out")),
			  cfg(scratch("config.conf")), ses(scratch("session.conf")),
			  hashcache(scratch("hashdb.cache"))
		{
			Samurai::IO::File::mkdir(dir.c_str());
			Samurai::IO::File::mkdir(out.c_str());
		}

		~Share()
		{
			shares = 0;
			core.reset();
			hashes.reset();

			Samurai::IO::File(hashcache).remove();
			Samurai::IO::File(hashcache + ".new").remove();
			for (size_t n = 0; n < madeFiles.size(); n++)
				Samurai::IO::File(madeFiles[n]).remove();

			Samurai::IO::File(out + "/files.xml").remove();
			Samurai::IO::File(out + "/files.xml.bz2").remove();
			Samurai::IO::File::rmdir(out.c_str());
			Samurai::IO::File::rmdir(dir.c_str());
			Samurai::IO::File(cfg).remove();
			Samurai::IO::File(ses).remove();
		}

		Share(const Share&) = delete;
		Share& operator=(const Share&) = delete;

		void file(const std::string& name, size_t size)
		{
			const std::string path = dir + "/" + name;
			/* Distinct contents, so every file gets a TTH of its own. */
			writeFile(path, name + std::string(size, 'x'));
			madeFiles.push_back(path);
		}

		bool scan()
		{
			writeFile(cfg,
				"[User]\nNickname=tester\n\n"
				"[Shares]\nTotal shares=1\nShare 0=shared:" + dir + "\n");

			core = std::make_unique<QuickDC::Core>(cfg, ses);
			hashes = std::make_unique<QuickDC::Hash::Manager>(hashcache.c_str());
			shares = new QuickDC::Share::Manager(core->config, hashes.get(), out.c_str());

			delete core->shares;
			core->shares = shares;

			shares->initialize();

			/* The search tables are filled as each TTH arrives, so nothing is
			   findable until the hashing has been round to all of it. */
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

		/* The TTH of a shared file, for searching by hash. */
		std::string tthOf(const char* name)
		{
			Replies r;
			Request req(&r);
			req.addInclusion(name);
			shares->search(&req);
			if (r.count() != 1 || !r.files[0] || !r.files[0]->getTTH())
				return std::string();
			return std::string(r.files[0]->getTTH());
		}
};

int main()
{
	setbuf(stdout, 0);

	/* A TTH root is 39 base32 characters, in upper case. */
	{
		Request req(0);
		CHECK(!req.isTTH());
		CHECK(!req.getTTH());

		req.setTTH("abcdefghijklmnopqrstuvwxyz234567abcdefg");
		CHECK(req.isTTH());
		CHECK(std::string(req.getTTH()) == "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFG");
		printf("  ok a TTH is kept in upper case\n");
	}

	/* Anything that is not one is not kept. */
	{
		Request a(0);
		a.setTTH("tooshort");
		CHECK(!a.isTTH());

		Request b(0);
		b.setTTH("abcdefghijklmnopqrstuvwxyz234567abcdefghij");
		CHECK(!b.isTTH());
		printf("  ok something that is not a TTH is not taken for one\n");
	}

	/*
	 * Replacing a good TTH with a bad one. The old string is freed either way,
	 * and leaving the pointer behind means the destructor frees it again.
	 */
	{
		Request req(0);
		req.setTTH("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFG");
		CHECK(req.isTTH());

		req.setTTH("rubbish");
		CHECK(!req.isTTH());
		CHECK(!req.getTTH());
		printf("  ok a TTH replaced by a bad one is dropped, not left dangling\n");
	}

	/* The size policy is what the request was given. */
	{
		Request req(0);
		CHECK(req.getSizePolicy() == Request::SizeAny);

		req.setSizePolicy(Request::SizeMin);
		CHECK(req.getSizePolicy() == Request::SizeMin);

		req.setSize(1024);
		CHECK(req.getSize() == 1024);

		req.setSizePolicy(Request::SizeExact);
		CHECK(req.getSizePolicy() == Request::SizeExact);
		printf("  ok the size policy is the one the request was given\n");
	}

	/* And so is the file type, which starts out meaning everything. */
	{
		Request req(0);
		CHECK(req.getFileType() == Request::SearchAll);

		req.setType(Request::SearchFiles);
		CHECK(req.getFileType() == Request::SearchFiles);

		req.setType(Request::SearchDirectories);
		CHECK(req.getFileType() == Request::SearchDirectories);
		printf("  ok the file type is the one the request was given\n");
	}

	/* The constructor that takes everything at once agrees with the setters. */
	{
		Request req(Request::SizeMax, 4096,
			"abcdefghijklmnopqrstuvwxyz234567abcdefg", "tok", 0);

		CHECK(req.getSizePolicy() == Request::SizeMax);
		CHECK(req.getSize() == 4096);
		CHECK(req.isTTH());
		CHECK(std::string(req.getToken()) == "tok");
		CHECK(req.getFileType() == Request::SearchAll);
		printf("  ok a request built in one go carries all of it\n");
	}

	/* The token can be replaced. */
	{
		Request req(0);
		CHECK(!req.getToken());
		req.setToken("first");
		CHECK(std::string(req.getToken()) == "first");
		req.setToken("second");
		CHECK(std::string(req.getToken()) == "second");
		printf("  ok the token can be replaced\n");
	}

	/* Now against a real share. */
	Share share;
	share.file("holiday photos.jpg", 1000);
	share.file("holiday video sample.avi", 5000);
	share.file("concert video.avi", 20000);
	share.file("readme.txt", 100);
	CHECK(share.scan());

	/* One term, matched anywhere in the name and in any case. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("concert");
		share.shares->search(&req);

		CHECK(r.count() == 1);
		CHECK(r.has("concert video.avi"));

		Replies upper;
		Request req2(&upper);
		req2.addInclusion("CONCERT");
		share.shares->search(&req2);
		CHECK(upper.count() == 1);
		printf("  ok a single term matches whatever case it is in\n");
	}

	/* Every term has to be there. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("holiday");
		req.addInclusion("video");
		share.shares->search(&req);

		CHECK(r.count() == 1);
		CHECK(r.has("holiday video sample.avi"));
		printf("  ok every term has to appear: %s\n", r.joined().c_str());
	}

	/*
	 * And the order they were typed in is not part of the question. The terms
	 * were matched one after another from where the last one was found, so
	 * asking for the same words backwards found nothing.
	 */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("video");
		req.addInclusion("holiday");
		share.shares->search(&req);

		CHECK(r.count() == 1);
		CHECK(r.has("holiday video sample.avi"));
		printf("  ok the terms may be given in any order\n");
	}

	/* A term nobody has. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("nosuchthing");
		share.shares->search(&req);
		CHECK(r.count() == 0);
		printf("  ok a term nothing matches returns nothing\n");
	}

	/* A search with no terms at all is not a search for everything. */
	{
		Replies r;
		Request req(&r);
		share.shares->search(&req);
		CHECK(r.count() == 0);
		printf("  ok a search with no terms does not return the whole share\n");
	}

	/*
	 * An exclusion excludes. The loop kept a file only when the excluded term
	 * was found in it, so a search for "video" without "sample" answered with
	 * the one file the asker had said they did not want, and hid the rest.
	 */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("video");
		req.addExclusion("sample");
		share.shares->search(&req);

		CHECK(r.has("concert video.avi"));
		CHECK(!r.has("holiday video sample.avi"));
		CHECK(r.count() == 1);
		printf("  ok an excluded term is excluded: %s\n", r.joined().c_str());
	}

	/* Several exclusions, any one of which is enough to drop a file. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("holiday");
		req.addExclusion("sample");
		req.addExclusion("photos");
		share.shares->search(&req);

		CHECK(r.count() == 0);
		printf("  ok any one exclusion is enough to drop a file\n");
	}

	/* The extension narrows it down. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("holiday");
		req.addExtension("avi");
		share.shares->search(&req);

		CHECK(r.count() == 1);
		CHECK(r.has("holiday video sample.avi"));
		printf("  ok an extension narrows the results\n");
	}

	/* An extension nothing has. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("holiday");
		req.addExtension("mp3");
		share.shares->search(&req);
		CHECK(r.count() == 0);
		printf("  ok an extension nothing has matches nothing\n");
	}

	/*
	 * A size limit is part of the question too. Both hubs set the policy on the
	 * request and the setter ignored it, so every incoming search was answered
	 * as though any size would do.
	 */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("avi");
		req.setSize(10000);
		req.setSizePolicy(Request::SizeMin);
		share.shares->search(&req);

		CHECK(r.has("concert video.avi"));
		CHECK(!r.has("holiday video sample.avi"));
		printf("  ok a smallest size is respected: %s\n", r.joined().c_str());
	}

	/* And the other way round. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("avi");
		req.setSize(10000);
		req.setSizePolicy(Request::SizeMax);
		share.shares->search(&req);

		CHECK(r.has("holiday video sample.avi"));
		CHECK(!r.has("concert video.avi"));
		printf("  ok a largest size is respected: %s\n", r.joined().c_str());
	}

	/* Any size means any size. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("avi");
		req.setSize(10000);
		req.setSizePolicy(Request::SizeAny);
		share.shares->search(&req);
		CHECK(r.count() == 2);
		printf("  ok any size matches both\n");
	}

	/* Searching by hash finds the one file and nothing else. */
	{
		const std::string tth = share.tthOf("readme.txt");
		CHECK(tth.size() == 39);

		Replies r;
		Request req(&r);
		req.setTTH(tth.c_str());
		CHECK(req.isTTH());
		share.shares->search(&req);

		CHECK(r.count() == 1);
		CHECK(r.has("readme.txt"));
		printf("  ok a search by hash finds the file it names\n");
	}

	/* A hash nobody has. */
	{
		Replies r;
		Request req(&r);
		req.setTTH("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
		share.shares->search(&req);
		CHECK(r.count() == 0);
		printf("  ok a hash nothing matches returns nothing\n");
	}

	/* No more than the request asks for. */
	{
		Replies r;
		Request req(&r);
		req.addInclusion("hol");
		share.shares->search(&req);
		CHECK((int) r.count() <= req.getMaxResults());
		printf("  ok no more results than the request allows\n");
	}

	/* A reply is routed to the search that asked for it, by its token. */
	{
		QuickDC::Share::SearchManager mgr(0);

		Results mine, theirs;
		Request a(0), b(0);
		a.setToken("mine");
		b.setToken("theirs");

		mgr.add(&a, &mine);
		mgr.add(&b, &theirs);

		QuickDC::Share::SearchResult result("file.txt", 100, 1, 0, 0, "mine");
		mgr.handle(&result);

		CHECK(mine.received == 1);
		CHECK(theirs.received == 0);
		printf("  ok a result goes to the search whose token it carries\n");
	}

	/* A result for a search nobody is running goes nowhere. */
	{
		QuickDC::Share::SearchManager mgr(0);

		Results mine;
		Request a(0);
		a.setToken("mine");
		mgr.add(&a, &mine);

		QuickDC::Share::SearchResult stray("file.txt", 100, 1, 0, 0, "somebody else");
		mgr.handle(&stray);
		CHECK(mine.received == 0);

		/* Nor does one carrying no token at all. */
		QuickDC::Share::SearchResult anonymous("file.txt", 100, 1, 0, 0, 0);
		mgr.handle(&anonymous);
		CHECK(mine.received == 0);
		printf("  ok a result for a search we are not running is dropped\n");
	}

	/* A search that has been taken off the list stops receiving. */
	{
		QuickDC::Share::SearchManager mgr(0);

		Results mine;
		Request a(0);
		a.setToken("mine");
		mgr.add(&a, &mine);
		mgr.remove(&a);

		QuickDC::Share::SearchResult result("file.txt", 100, 1, 0, 0, "mine");
		mgr.handle(&result);
		CHECK(mine.received == 0);
		printf("  ok a search that was removed stops receiving results\n");
	}

	/* And the manager keeps nothing of the searches it is done with. */
	{
		Request a(0), b(0);
		a.setToken("a");
		b.setToken("b");

		Results ra, rb;

		const size_t before = live;
		{
			QuickDC::Share::SearchManager mgr(0);
			mgr.add(&a, &ra);
			mgr.add(&b, &rb);
			mgr.remove(&a);
			/* 'b' is still running when the manager goes. */
		}
		CHECK(live == before);
		printf("  ok the search manager frees what it allocated\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
