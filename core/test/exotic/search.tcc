/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * A search arrives from a hub, is turned into a SearchRequest, and is run
 * against our own share-> What comes back names our files to whoever asked, so
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

#include <thread>
#include <samurai/io/file.h>

#include <chrono>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <samurai/posix.h>
#include <vector>

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
		void EventSearchReply(QuickDC::Share::SearchReply* reply) override
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
		std::vector<std::string> madeDirs;
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

			/* Deepest first, and with rmdir: remove() unlinks, which never
			   takes a directory away. */
			for (size_t n = madeDirs.size(); n-- > 0; )
				Samurai::IO::File::rmdir(madeDirs[n].c_str());

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

		void subdir(const std::string& name)
		{
			const std::string path = dir + "/" + name;
			Samurai::IO::File::mkdir(path.c_str());
			madeDirs.push_back(path);
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
				std::this_thread::sleep_for(std::chrono::microseconds(1000));
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

/* A TTH root is 39 base32 characters, in upper case. */

namespace {

/*
 * Built once for this file rather than per case: scanning and hashing a
 * directory is the expensive part, and every case below searches the same four
 * files. Under DISCOVER each test is its own process, so this is the cost of a
 * test rather than of the file - see core/test/exotic/CMakeLists.txt.
 */
Share* share = 0;

}

EXO_SETUP({
	share = new Share();
	share->file("holiday photos.jpg", 1000);
	share->file("holiday video sample.avi", 5000);
	share->file("concert video.avi", 20000);
	share->file("readme.txt", 100);
	return share->scan() ? 1 : 0;
});

EXO_TEARDOWN({
	delete share;
	share = 0;
	return 1;
});

EXO_TEST(search_a_tth_is_kept_in_upper_case,
{
		Request req(0);
		EXO_ASSERT(!req.isTTH());
		EXO_ASSERT(!req.getTTH());

		req.setTTH("abcdefghijklmnopqrstuvwxyz234567abcdefg");
		EXO_ASSERT(req.isTTH());
		EXO_ASSERT(std::string(req.getTTH()) == "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFG");
	
	return 1;
});

/* Anything that is not one is not kept. */
EXO_TEST(search_something_that_is_not_a_tth_is_not_taken_for_one,
{
		Request a(0);
		a.setTTH("tooshort");
		EXO_ASSERT(!a.isTTH());

		Request b(0);
		b.setTTH("abcdefghijklmnopqrstuvwxyz234567abcdefghij");
		EXO_ASSERT(!b.isTTH());
	
	return 1;
});

/*
	 * Replacing a good TTH with a bad one. The old string is freed either way,
	 * and leaving the pointer behind means the destructor frees it again.
	 */
EXO_TEST(search_a_tth_replaced_by_a_bad_one_is_dropped_not_left,
{
		Request req(0);
		req.setTTH("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFG");
		EXO_ASSERT(req.isTTH());

		req.setTTH("rubbish");
		EXO_ASSERT(!req.isTTH());
		EXO_ASSERT(!req.getTTH());
	
	return 1;
});

/* The size policy is what the request was given. */
EXO_TEST(search_the_size_policy_is_the_one_the_request_was_given,
{
		Request req(0);
		EXO_ASSERT(req.getSizePolicy() == Request::SizeAny);

		req.setSizePolicy(Request::SizeMin);
		EXO_ASSERT(req.getSizePolicy() == Request::SizeMin);

		req.setSize(1024);
		EXO_ASSERT(req.getSize() == 1024);

		req.setSizePolicy(Request::SizeExact);
		EXO_ASSERT(req.getSizePolicy() == Request::SizeExact);
	
	return 1;
});

/* And so is the file type, which starts out meaning everything. */
EXO_TEST(search_the_file_type_is_the_one_the_request_was_given,
{
		Request req(0);
		EXO_ASSERT(req.getFileType() == Request::SearchAll);

		req.setType(Request::SearchFiles);
		EXO_ASSERT(req.getFileType() == Request::SearchFiles);

		req.setType(Request::SearchDirectories);
		EXO_ASSERT(req.getFileType() == Request::SearchDirectories);
	
	return 1;
});

/* The constructor that takes everything at once agrees with the setters. */
EXO_TEST(search_a_request_built_in_one_go_carries_all_of_it,
{
		Request req(Request::SizeMax, 4096,
			"abcdefghijklmnopqrstuvwxyz234567abcdefg", "tok", 0);

		EXO_ASSERT(req.getSizePolicy() == Request::SizeMax);
		EXO_ASSERT(req.getSize() == 4096);
		EXO_ASSERT(req.isTTH());
		EXO_ASSERT(std::string(req.getToken()) == "tok");
		EXO_ASSERT(req.getFileType() == Request::SearchAll);
	
	return 1;
});

/* The token can be replaced. */
EXO_TEST(search_the_token_can_be_replaced,
{
		Request req(0);
		EXO_ASSERT(!req.getToken());
		req.setToken("first");
		EXO_ASSERT(std::string(req.getToken()) == "first");
		req.setToken("second");
		EXO_ASSERT(std::string(req.getToken()) == "second");
	
	return 1;
});

/* One term, matched anywhere in the name and in any case. */
EXO_TEST(search_a_single_term_matches_whatever_case_it_is_in,
{
		Replies r;
		Request req(&r);
		req.addInclusion("concert");
		share->shares->search(&req);

		EXO_ASSERT(r.count() == 1);
		EXO_ASSERT(r.has("concert video.avi"));

		Replies upper;
		Request req2(&upper);
		req2.addInclusion("CONCERT");
		share->shares->search(&req2);
		EXO_ASSERT(upper.count() == 1);
	
	return 1;
});

/* Every term has to be there. */
EXO_TEST(search_every_term_has_to_appear,
{
		Replies r;
		Request req(&r);
		req.addInclusion("holiday");
		req.addInclusion("video");
		share->shares->search(&req);

		EXO_ASSERT(r.count() == 1);
		EXO_ASSERT(r.has("holiday video sample.avi"));
	
	return 1;
});

/*
	 * And the order they were typed in is not part of the question. The terms
	 * were matched one after another from where the last one was found, so
	 * asking for the same words backwards found nothing.
	 */
EXO_TEST(search_the_terms_may_be_given_in_any_order,
{
		Replies r;
		Request req(&r);
		req.addInclusion("video");
		req.addInclusion("holiday");
		share->shares->search(&req);

		EXO_ASSERT(r.count() == 1);
		EXO_ASSERT(r.has("holiday video sample.avi"));
	
	return 1;
});

/* A term nobody has. */
EXO_TEST(search_a_term_nothing_matches_returns_nothing,
{
		Replies r;
		Request req(&r);
		req.addInclusion("nosuchthing");
		share->shares->search(&req);
		EXO_ASSERT(r.count() == 0);
	
	return 1;
});

/* A search with no terms at all is not a search for everything. */
EXO_TEST(search_a_search_with_no_terms_does_not_return_the_whole_share,
{
		Replies r;
		Request req(&r);
		share->shares->search(&req);
		EXO_ASSERT(r.count() == 0);
	
	return 1;
});

/*
	 * An exclusion excludes. The loop kept a file only when the excluded term
	 * was found in it, so a search for "video" without "sample" answered with
	 * the one file the asker had said they did not want, and hid the rest.
	 */
EXO_TEST(search_an_excluded_term_is_excluded,
{
		Replies r;
		Request req(&r);
		req.addInclusion("video");
		req.addExclusion("sample");
		share->shares->search(&req);

		EXO_ASSERT(r.has("concert video.avi"));
		EXO_ASSERT(!r.has("holiday video sample.avi"));
		EXO_ASSERT(r.count() == 1);
	
	return 1;
});

/* Several exclusions, any one of which is enough to drop a file. */
EXO_TEST(search_any_one_exclusion_is_enough_to_drop_a_file,
{
		Replies r;
		Request req(&r);
		req.addInclusion("holiday");
		req.addExclusion("sample");
		req.addExclusion("photos");
		share->shares->search(&req);

		EXO_ASSERT(r.count() == 0);
	
	return 1;
});

/* The extension narrows it down. */
EXO_TEST(search_an_extension_narrows_the_results,
{
		Replies r;
		Request req(&r);
		req.addInclusion("holiday");
		req.addExtension("avi");
		share->shares->search(&req);

		EXO_ASSERT(r.count() == 1);
		EXO_ASSERT(r.has("holiday video sample.avi"));
	
	return 1;
});

/* An extension nothing has. */
EXO_TEST(search_an_extension_nothing_has_matches_nothing,
{
		Replies r;
		Request req(&r);
		req.addInclusion("holiday");
		req.addExtension("mp3");
		share->shares->search(&req);
		EXO_ASSERT(r.count() == 0);
	
	return 1;
});

/*
	 * A size limit is part of the question too. Both hubs set the policy on the
	 * request and the setter ignored it, so every incoming search was answered
	 * as though any size would do.
	 */
EXO_TEST(search_a_smallest_size_is_respected,
{
		Replies r;
		Request req(&r);
		req.addInclusion("avi");
		req.setSize(10000);
		req.setSizePolicy(Request::SizeMin);
		share->shares->search(&req);

		EXO_ASSERT(r.has("concert video.avi"));
		EXO_ASSERT(!r.has("holiday video sample.avi"));
	
	return 1;
});

/* And the other way round. */
EXO_TEST(search_a_largest_size_is_respected,
{
		Replies r;
		Request req(&r);
		req.addInclusion("avi");
		req.setSize(10000);
		req.setSizePolicy(Request::SizeMax);
		share->shares->search(&req);

		EXO_ASSERT(r.has("holiday video sample.avi"));
		EXO_ASSERT(!r.has("concert video.avi"));
	
	return 1;
});

/* Any size means any size. */
EXO_TEST(search_any_size_matches_both,
{
		Replies r;
		Request req(&r);
		req.addInclusion("avi");
		req.setSize(10000);
		req.setSizePolicy(Request::SizeAny);
		share->shares->search(&req);
		EXO_ASSERT(r.count() == 2);
	
	return 1;
});

/* Searching by hash finds the one file and nothing else. */
EXO_TEST(search_a_search_by_hash_finds_the_file_it_names,
{
		const std::string tth = share->tthOf("readme.txt");
		EXO_ASSERT(tth.size() == 39);

		Replies r;
		Request req(&r);
		req.setTTH(tth.c_str());
		EXO_ASSERT(req.isTTH());
		share->shares->search(&req);

		EXO_ASSERT(r.count() == 1);
		EXO_ASSERT(r.has("readme.txt"));
	
	return 1;
});

/* A hash nobody has. */
EXO_TEST(search_a_hash_nothing_matches_returns_nothing,
{
		Replies r;
		Request req(&r);
		req.setTTH("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
		share->shares->search(&req);
		EXO_ASSERT(r.count() == 0);
	
	return 1;
});

/*
	 * What a hit is called. The name goes out in the FN field of a search
	 * result - by UDP straight to whoever searched, or through the hub - so it
	 * has to be the name the file has inside the share->
	 *
	 * A subdirectory is constructed from its full path on disk, and the public
	 * name was composed from that, so every hit below the top level of a share
	 * used to hand out an absolute local path: the account name in the home
	 * directory, and the layout of the machine, to anyone who searched for a
	 * common word.
	 */
EXO_TEST(search_a_hit_below_the_top_level_is_named,
{
		Share nested;
		nested.subdir("albums");
		nested.subdir("albums/1999");
		nested.file("readme.txt", 10);
		nested.file("albums/1999/single.mp3", 20);
		EXO_ASSERT(nested.scan());

		Replies top;
		Request wanted(&top);
		wanted.addInclusion("readme");
		nested.shares->search(&wanted);

		EXO_ASSERT(top.count() == 1);
		if (top.count() == 1)
			EXO_ASSERT(top.names[0] == "/shared/readme.txt");

		Replies deep;
		Request req(&deep);
		req.addInclusion("single.mp3");
		nested.shares->search(&req);

		EXO_ASSERT(deep.count() == 1);
		if (deep.count() == 1)
		{
			const std::string fn = deep.names[0];

			EXO_ASSERT(fn == "/shared/albums/1999/single.mp3");
			EXO_ASSERT(fn.find(nested.dir) == std::string::npos);
			EXO_ASSERT(fn.find("/tmp") == std::string::npos);

			/* And the name handed out is the one a $Get for it resolves, since
			   that is all a peer has to ask by. */
			Samurai::IO::File* served = nested.shares->getFile(fn.c_str());
			EXO_ASSERT(served != 0);
			if (served)
				EXO_ASSERT(served->getName() == deep.files[0]->getFile().getName());

		}
	
	return 1;
});

/* No more than the request asks for. */
EXO_TEST(search_no_more_results_than_the_request_allows,
{
		Replies r;
		Request req(&r);
		req.addInclusion("hol");
		share->shares->search(&req);
		EXO_ASSERT((int) r.count() <= req.getMaxResults());
	
	return 1;
});

/* A result quoting a token goes to the search that went out with it. */
EXO_TEST(search_a_result_goes_to_the_search_whose_token_it_carries,
{
		QuickDC::Share::SearchManager mgr;

		Request a(0), b(0);
		a.setToken("mine");
		a.addInclusion("film");
		b.setToken("theirs");
		b.addInclusion("film");

		const uint64_t mine = mgr.add(a);
		const uint64_t theirs = mgr.add(b);
		EXO_ASSERT(mine && theirs && mine != theirs);

		QuickDC::Share::SearchResult toMine("film.mkv", 100, 1, 0, 0, "mine");
		QuickDC::Share::SearchResult toTheirs("film.mkv", 100, 1, 0, 0, "theirs");
		EXO_ASSERT(mgr.attribute(toMine) == mine);
		EXO_ASSERT(mgr.attribute(toTheirs) == theirs);

	return 1;
});

/* A token no live search went out with is nobody's, whatever the name says. */
EXO_TEST(search_a_result_quoting_a_strange_token_is_dropped,
{
		QuickDC::Share::SearchManager mgr;

		Request a(0);
		a.setToken("mine");
		a.addInclusion("file");
		mgr.add(a);

		QuickDC::Share::SearchResult stray("file.txt", 100, 1, 0, 0, "somebody else");
		EXO_ASSERT(mgr.attribute(stray) == 0);

	return 1;
});

/* Without a token, as on NMDC, the name has to hold every word of the search, in any case. */
EXO_TEST(search_a_result_without_a_token_is_placed_by_its_name,
{
		QuickDC::Share::SearchManager mgr;

		Request a(0);
		a.setToken("t");
		a.addInclusion("holiday");
		a.addInclusion("2019");
		const uint64_t holiday = mgr.add(a);

		QuickDC::Share::SearchResult both("Photos/2019 HOLIDAY/beach.jpg", 100, 1);
		QuickDC::Share::SearchResult one("Photos/holiday/beach.jpg", 100, 1);
		EXO_ASSERT(mgr.attribute(both) == holiday);
		EXO_ASSERT(mgr.attribute(one) == 0);

	return 1;
});

/* Nor may it hold a word the search excluded. */
EXO_TEST(search_a_name_holding_an_excluded_word_answers_no_search,
{
		QuickDC::Share::SearchManager mgr;

		Request a(0);
		a.addInclusion("concert");
		a.addExclusion("bootleg");
		const uint64_t concert = mgr.add(a);

		QuickDC::Share::SearchResult clean("concert.flac", 100, 1);
		QuickDC::Share::SearchResult excluded("concert (Bootleg).flac", 100, 1);
		EXO_ASSERT(mgr.attribute(clean) == concert);
		EXO_ASSERT(mgr.attribute(excluded) == 0);

	return 1;
});

/* A name two searches would take goes to the newer; each keeps what only it matches. */
EXO_TEST(search_two_searches_at_once_each_get_their_own,
{
		QuickDC::Share::SearchManager mgr;

		Request older(0), newer(0);
		older.addInclusion("linux");
		newer.addInclusion("linux");
		newer.addInclusion("iso");
		newer.addExclusion("beta");
		const uint64_t first = mgr.add(older);
		const uint64_t second = mgr.add(newer);

		QuickDC::Share::SearchResult iso("linux-6.1.iso", 100, 1);
		QuickDC::Share::SearchResult tarball("linux-6.1.tar.xz", 100, 1);
		QuickDC::Share::SearchResult beta("linux-beta.iso", 100, 1);
		EXO_ASSERT(mgr.attribute(iso) == second);
		EXO_ASSERT(mgr.attribute(tarball) == first);
		EXO_ASSERT(mgr.attribute(beta) == first);

	return 1;
});

/* A search taken off the list is answered no more, by token or by name. */
EXO_TEST(search_a_forgotten_search_is_answered_no_more,
{
		QuickDC::Share::SearchManager mgr;

		Request a(0);
		a.setToken("mine");
		a.addInclusion("file");
		const uint64_t mine = mgr.add(a);
		EXO_ASSERT(mgr.remove(mine));
		EXO_ASSERT(!mgr.remove(mine));

		QuickDC::Share::SearchResult late("file.txt", 100, 1, 0, 0, "mine");
		QuickDC::Share::SearchResult untokened("file.txt", 100, 1);
		EXO_ASSERT(mgr.attribute(late) == 0);
		EXO_ASSERT(mgr.attribute(untokened) == 0);
		EXO_ASSERT(mgr.live() == 0);

	return 1;
});

/* Searches nobody forgets are let go oldest first, and numbers are never reused. */
EXO_TEST(search_the_oldest_live_search_goes_beyond_the_limit,
{
		QuickDC::Share::SearchManager mgr;

		std::vector<uint64_t> ids;
		for (int n = 0; n <= QUICKDC_SEARCHES_LIVE; ++n)
		{
			Request request(0);
			request.setToken(("t" + std::to_string(n)).c_str());
			request.addInclusion("x");
			ids.push_back(mgr.add(request));
		}

		EXO_ASSERT(mgr.live() == QUICKDC_SEARCHES_LIVE);
		QuickDC::Share::SearchResult oldest("x", 1, 1, 0, 0, "t0");
		QuickDC::Share::SearchResult newest("x", 1, 1, 0, 0, ("t" + std::to_string(QUICKDC_SEARCHES_LIVE)).c_str());
		EXO_ASSERT(mgr.attribute(oldest) == 0);
		EXO_ASSERT(mgr.attribute(newest) == ids.back());
		EXO_ASSERT(!mgr.remove(ids.front()));

		Request again(0);
		EXO_ASSERT(mgr.add(again) > ids.back());

	return 1;
});
