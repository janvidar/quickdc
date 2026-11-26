/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The public hub directory: its sources as the configuration names them, the
 * merge of what they list, the cache of each in the shape the interface wrote it
 * before the core kept the directory, and when a source is fetched again.
 *
 * Nothing here reaches another host. A fetch that must fail knocks on a closed
 * port of the loopback address; a list that arrives whole is the hublist suite's
 * and the qdc scenario's to test.
 */

#include "quickdc.h"
#include "config/preferences.h"
#include "network/hubdirectory.h"

#include <samurai/eventloop.h>
#include <samurai/io/net/socketmonitor.h>

#include <samurai/posix.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

static std::unique_ptr<Samurai::EventLoop> loop;

EXO_SETUP({
	loop = std::make_unique<Samurai::EventLoop>();
	return loop->isValid() ? 1 : 0;
});

EXO_TEARDOWN({
	loop.reset();
	return 1;
});

namespace {

const uint64_t NOW = 1769000000;

/* A directory of its own per case, gone with it. */
class Scratch {
	public:
		Scratch()
		{
			static int counter = 0;
			path = "/tmp/quickdc_hubdirectory_" + std::to_string((int) getpid()) + "_" + std::to_string(counter++);
			std::filesystem::create_directories(path);
		}

		~Scratch()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}

		Scratch(const Scratch&) = delete;
		Scratch& operator=(const Scratch&) = delete;

		std::string conf() const { return path + "/quickdc.conf"; }
		std::string cache() const { return path + "/hublists"; }

		void write(const std::string& file, const std::string& content) const
		{
			std::filesystem::create_directories(std::filesystem::path(file).parent_path());
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			out << content;
		}

		void configure(const std::vector<std::string>& urls) const
		{
			std::string text = "[Hublists]\nTotal=" + std::to_string(urls.size()) + "\n";
			for (size_t n = 0; n < urls.size(); ++n) text += "List " + std::to_string(n) + "=" + urls[n] + "\n";
			write(conf(), text);
		}

		std::string path;
};

/* What changed, as the service hears it. */
struct Told {
	int sources = 0;
	int hubs = 0;
};

std::unique_ptr<QuickDC::HubDirectory> directory(QuickDC::Preferences& prefs, const Scratch& scratch, Told& told)
{
	return std::make_unique<QuickDC::HubDirectory>(*loop, prefs, scratch.cache(),
		[&told] { told.sources++; }, [&told] { told.hubs++; });
}

/* A cache as the interface's QJsonDocument wrote one: compact, its keys in order. */
std::string cached(const std::string& url, uint64_t fetched, const std::string& hubs)
{
	return "{\"fetched\":" + std::to_string(fetched) + ",\"hubs\":[" + hubs + "],\"url\":\"" + url + "\"}";
}

std::string hub(const std::string& address, const std::string& name, uint64_t users = 0,
                const std::string& description = std::string(), const std::string& country = std::string())
{
	std::string out = "{\"address\":\"" + address + "\"";
	if (!country.empty()) out += ",\"country\":\"" + country + "\"";
	if (!description.empty()) out += ",\"description\":\"" + description + "\"";
	if (!name.empty()) out += ",\"name\":\"" + name + "\"";
	if (users) out += ",\"users\":" + std::to_string(users);
	return out + "}";
}

const QuickDC::Api::DirectoryHub* named(const QuickDC::HubDirectory& dir, const std::string& name)
{
	for (const QuickDC::Api::DirectoryHub& one : dir.hubs())
		if (one.hub.name == name) return &one;
	return nullptr;
}

template<typename Fn>
bool pump(Fn done, int timeout_ms = 15000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		loop->monitor().wait(10);
	}
	return true;
}

}

/* Written out the first time so they can be found and edited; a Total of nought is respected. */
EXO_TEST(hubdirectory_no_configuration_means_the_defaults_written_out,
{
	Scratch scratch;
	Told told;
	{
		QuickDC::Preferences prefs(scratch.conf());
		auto dir = directory(prefs, scratch, told);

		const std::vector<QuickDC::Api::HubListSource> sources = dir->sources();
		const std::vector<std::string> defaults = QuickDC::HubDirectory::defaultSources();
		EXO_ASSERT(sources.size() == defaults.size());
		for (size_t n = 0; n < sources.size() && n < defaults.size(); ++n)
			EXO_ASSERT(sources[n].url == defaults[n]);
		EXO_ASSERT(prefs.peekNumber("Hublists", "Total", -1) == static_cast<int>(defaults.size()));
		EXO_ASSERT(prefs.peekString("Hublists", "List 0", "") == defaults[0]);
		EXO_ASSERT(dir->hubs().empty());
	}

	scratch.write(scratch.conf(), "[Hublists]\nTotal=0\n");
	QuickDC::Preferences prefs(scratch.conf());
	auto dir = directory(prefs, scratch, told);
	EXO_ASSERT(dir->sources().empty());
	EXO_ASSERT(told.sources == 0 && told.hubs == 0);
	return 1;
});

/* In the order stored; a gap in the numbering or a list named twice is skipped. */
EXO_TEST(hubdirectory_sources_are_read_in_order,
{
	Scratch scratch;
	scratch.write(scratch.conf(), "[Hublists]\nTotal=4\nList 0=http://second.example.org/hublist.xml\n"
	              "List 2= http://first.example.org/hublist.xml \nList 3=http://second.example.org/hublist.xml\n");

	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);

	const std::vector<QuickDC::Api::HubListSource> sources = dir->sources();
	EXO_ASSERT(sources.size() == 2);
	if (sources.size() != 2) return 0;
	EXO_ASSERT(sources[0].url == "http://second.example.org/hublist.xml");
	EXO_ASSERT(sources[1].url == "http://first.example.org/hublist.xml");
	EXO_ASSERT(sources[0].fetched == 0 && !sources[0].fetching && !sources[0].cached && sources[0].error.empty());
	return 1;
});

/* Taken out of a stored configuration, and the file rewritten without it or the tail it leaves. */
EXO_TEST(hubdirectory_a_withdrawn_list_is_taken_out_of_the_configuration,
{
	const std::vector<std::string> retired = QuickDC::HubDirectory::retiredSources();
	EXO_ASSERT(!retired.empty());

	for (const std::string& gone : retired)
	{
		const std::vector<std::string> defaults = QuickDC::HubDirectory::defaultSources();
		EXO_ASSERT(std::find(defaults.begin(), defaults.end(), gone) == defaults.end());
	}

	Scratch scratch;
	scratch.configure({"http://mine.example.org/hublist.xml", retired[0], "http://other.example.org/x.xml"});
	{
		QuickDC::Preferences prefs(scratch.conf());
		Told told;
		auto dir = directory(prefs, scratch, told);
		EXO_ASSERT(dir->sources().size() == 2);
	}

	QuickDC::Preferences reread(scratch.conf());
	EXO_ASSERT(reread.peekNumber("Hublists", "Total", 0) == 2);
	EXO_ASSERT(reread.peekString("Hublists", "List 0", "") == "http://mine.example.org/hublist.xml");
	EXO_ASSERT(reread.peekString("Hublists", "List 1", "") == "http://other.example.org/x.xml");
	EXO_ASSERT(!reread.haveSetting("Hublists", "List 2"));
	return 1;
});

/* The cache file is named by the SHA-1 of the address, as the interface named it. */
EXO_TEST(hubdirectory_a_cache_is_named_by_the_digest_of_its_address,
{
	EXO_ASSERT(QuickDC::HubDirectory::cacheFile("/d", "abc") == "/d/a9993e364706816aba3e25717850c26c9cd0d89d.json");
	return 1;
});

/*
 * One row per hub however its lists spell it: the earlier list's description,
 * filled in from the later one, the larger user count, and both lists credited.
 */
EXO_TEST(hubdirectory_several_lists_merge_into_one,
{
	const std::string first = "http://a.example.org/hublist.xml";
	const std::string second = "http://b.example.org/hublist.xml";

	Scratch scratch;
	scratch.configure({first, second});
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), first), cached(first, NOW,
		hub("adcs://alpha.example.org:412", "Alpha", 10, "The first hub") + ","
		+ hub("beta.example.org", "Beta", 5) + ","
		+ hub("dchub://Rotated.Example.org", "Rotated")));
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), second), cached(second, NOW,
		hub("dchub://BETA.example.org:411/", "", 50, "The second hub", "NO") + ","
		+ hub("dchub://gamma.example.org:411", "Gamma", 1) + ","
		+ hub("rotated.example.org:411", "Again")));

	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);

	EXO_ASSERT(dir->hubs().size() == 4);
	EXO_ASSERT(dir->mergedFrom() == (std::vector<std::string>{first, second}));

	const QuickDC::Api::DirectoryHub* beta = named(*dir, "Beta");
	EXO_ASSERT(beta != nullptr);
	if (!beta) return 0;
	EXO_ASSERT(beta->hub.address == "beta.example.org");
	EXO_ASSERT(beta->hub.description == "The second hub");
	EXO_ASSERT(beta->hub.country == "NO");
	EXO_ASSERT(beta->hub.users == 50);
	EXO_ASSERT(beta->listedBy == (std::vector<uint32_t>{0, 1}));

	EXO_ASSERT(named(*dir, "Rotated") && named(*dir, "Rotated")->listedBy.size() == 2);
	EXO_ASSERT(named(*dir, "Again") == nullptr);
	EXO_ASSERT(named(*dir, "Gamma") && named(*dir, "Gamma")->listedBy == std::vector<uint32_t>{1});

	const std::vector<QuickDC::Api::HubListSource> sources = dir->sources();
	EXO_ASSERT(sources[0].hubs == 3 && sources[0].cached && sources[0].fetched == NOW);
	return 1;
});

/* The address that says which certificate to expect wins, whichever list named the hub first. */
EXO_TEST(hubdirectory_a_hub_keeps_the_address_with_the_keyprint,
{
	const std::string first = "http://a.example.org/hublist.xml";
	const std::string second = "http://b.example.org/hublist.xml";
	const std::string pinned = "adcs://secure.example.org:1511/?kp=SHA256/ABCDEF";

	Scratch scratch;
	scratch.configure({first, second});
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), first),
		cached(first, NOW, hub("adcs://secure.example.org:1511", "Secure") + ","
		+ hub("adcs://rotate.example.org:412/?kp=SHA256/OLDOLDOLD", "Rotate")));
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), second),
		cached(second, NOW, hub(pinned, "Secure elsewhere") + "," + hub("adc://kp.example.org:1", "Kp") + ","
		+ hub("adcs://rotate.example.org:412/?kp=SHA256/NEWNEWNEW", "Rotate again")));

	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);

	const QuickDC::Api::DirectoryHub* secure = named(*dir, "Secure");
	EXO_ASSERT(secure && secure->hub.address == pinned);

	/* Between two keyprints the first list's: nothing here knows which certificate is current. */
	const QuickDC::Api::DirectoryHub* rotate = named(*dir, "Rotate");
	EXO_ASSERT(rotate && rotate->hub.address == "adcs://rotate.example.org:412/?kp=SHA256/OLDOLDOLD");
	EXO_ASSERT(dir->hubs().size() == 3);
	return 1;
});

/* What the interface's writer put in a cache - escapes, other scripts, byte counts past 2^53 - reads back as it was. */
EXO_TEST(hubdirectory_a_cache_the_interface_wrote_reads_whole,
{
	const std::string url = "https://te-home.net/?do=hublist&get=hublist.xml.bz2";

	Scratch scratch;
	scratch.configure({url});
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), url),
		"{\"fetched\":1769000000,\"hubs\":[{\"address\":\"dchub://one.example.org:411\",\"country\":\"NO\","
		"\"description\":\"Caf\\u00e9 \\\"quoted\\\" \\ud83d\\ude00 \xc3\xa6\\n\",\"maxusers\":500,"
		"\"name\":\"\\u0001One\",\"shared\":123456789012345678,\"users\":42},{\"name\":\"no address\"},"
		"{\"address\":\"adc://two.example.org:2780\"}],\"url\":\"" + url + "\"}");

	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);

	EXO_ASSERT(dir->hubs().size() == 2);
	if (dir->hubs().size() != 2) return 0;

	const QuickDC::HubListEntry& one = dir->hubs()[0].hub;
	EXO_ASSERT(one.name == "\x01One");
	EXO_ASSERT(one.description == "Caf\xc3\xa9 \"quoted\" \xf0\x9f\x98\x80 \xc3\xa6\n");
	EXO_ASSERT(one.country == "NO");
	EXO_ASSERT(one.users == 42);
	EXO_ASSERT(one.shared == 123456789012345678ULL);
	EXO_ASSERT(one.maxusers == 500);
	EXO_ASSERT(dir->sources()[0].fetched == 1769000000);
	return 1;
});

/* A file that says it is another source's, or does not parse, is no cache of this one. */
EXO_TEST(hubdirectory_a_cache_for_another_source_or_cut_short_is_ignored,
{
	const std::string mine = "http://mine.example.org/hublist.xml";
	const std::string other = "http://other.example.org/hublist.xml";

	Scratch scratch;
	scratch.configure({mine, other});
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), mine),
		cached("http://theirs.example.org/hublist.xml", NOW, hub("dchub://x.example.org:411", "X")));
	const std::string whole = cached(other, NOW, hub("dchub://y.example.org:411", "Y"));
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), other), whole.substr(0, whole.size() - 3));

	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);

	EXO_ASSERT(dir->hubs().empty());
	EXO_ASSERT(dir->sources()[0].fetched == 0 && dir->sources()[1].fetched == 0);
	return 1;
});

/* Nothing is fetched while the cache is fresh; a stale or empty one is, and force fetches all. */
EXO_TEST(hubdirectory_a_fresh_cache_is_not_fetched_again,
{
	const std::string fresh = "http://127.0.0.1:1/fresh.xml";
	const std::string stale = "http://127.0.0.1:1/stale.xml";
	const std::string empty = "http://127.0.0.1:1/empty.xml";

	Scratch scratch;
	scratch.configure({fresh, stale, empty});
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), fresh),
		cached(fresh, NOW - 3600, hub("dchub://f.example.org:411", "F")));
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), stale),
		cached(stale, NOW - QuickDC::HubDirectory::MAX_AGE_HOURS * 3600, hub("dchub://s.example.org:411", "S")));
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), empty), cached(empty, NOW - 60, ""));

	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);

	dir->refresh(false, NOW);
	std::vector<QuickDC::Api::HubListSource> sources = dir->sources();
	EXO_ASSERT(!sources[0].fetching && sources[1].fetching && sources[2].fetching);
	EXO_ASSERT(told.sources == 1);

	EXO_ASSERT(pump([&] { return !dir->sources()[1].fetching && !dir->sources()[2].fetching; }));
	sources = dir->sources();
	EXO_ASSERT(!sources[1].error.empty());

	/* What it had is kept: a list that could not be fetched is not a list of no hubs. */
	EXO_ASSERT(sources[1].hubs == 1 && named(*dir, "S") != nullptr);

	dir->refresh(true, NOW);
	sources = dir->sources();
	EXO_ASSERT(sources[0].fetching && sources[1].fetching && sources[2].fetching);
	EXO_ASSERT(pump([&] {
		for (const QuickDC::Api::HubListSource& s : dir->sources()) if (s.fetching) return false;
		return true;
	}));
	return 1;
});

/* A source that was just asked is left alone, however that went, until the retry interval passes. */
EXO_TEST(hubdirectory_a_failed_source_is_asked_again_only_after_a_while,
{
	const std::string url = "http://127.0.0.1:1/down.xml";

	Scratch scratch;
	scratch.configure({url});
	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);

	dir->refresh(false, NOW);
	EXO_ASSERT(dir->sources()[0].fetching);
	EXO_ASSERT(pump([&] { return !dir->sources()[0].fetching; }));
	EXO_ASSERT(!dir->sources()[0].error.empty());

	const int before = told.sources;
	dir->refresh(false, NOW + 60);
	EXO_ASSERT(!dir->sources()[0].fetching);
	EXO_ASSERT(told.sources == before);

	dir->refresh(false, NOW + QuickDC::HubDirectory::RETRY_MINUTES * 60);
	EXO_ASSERT(dir->sources()[0].fetching);
	EXO_ASSERT(pump([&] { return !dir->sources()[0].fetching; }));
	return 1;
});

/* Added, a source's cache is read and the list stored; removed, its hubs and its cache go. */
EXO_TEST(hubdirectory_a_source_is_added_removed_and_reset,
{
	const std::string kept = "http://kept.example.org/hublist.xml";
	const std::string added = "http://127.0.0.1:1/added.xml";

	Scratch scratch;
	scratch.configure({kept});
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), kept),
		cached(kept, NOW, hub("dchub://k.example.org:411", "K")));
	scratch.write(QuickDC::HubDirectory::cacheFile(scratch.cache(), added),
		cached(added, NOW, hub("dchub://a.example.org:411", "A") + "," + hub("dchub://k.example.org", "K again")));

	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);
	EXO_ASSERT(dir->hubs().size() == 1);

	EXO_ASSERT(dir->add(" " + added + " ", NOW));
	EXO_ASSERT(!dir->add(added, NOW));
	EXO_ASSERT(dir->sources().size() == 2);

	/* Fresh in the cache, so shown at once and not fetched. */
	EXO_ASSERT(!dir->sources()[1].fetching);
	EXO_ASSERT(dir->hubs().size() == 2);
	EXO_ASSERT(named(*dir, "K")->listedBy == (std::vector<uint32_t>{0, 1}));
	EXO_ASSERT(prefs.peekNumber("Hublists", "Total", 0) == 2);
	EXO_ASSERT(prefs.peekString("Hublists", "List 1", "") == added);

	const int hubsTold = told.hubs;
	EXO_ASSERT(dir->remove(added));
	EXO_ASSERT(!dir->remove(added));
	EXO_ASSERT(told.hubs == hubsTold + 1);
	EXO_ASSERT(dir->hubs().size() == 1);
	EXO_ASSERT(!std::filesystem::exists(QuickDC::HubDirectory::cacheFile(scratch.cache(), added)));
	EXO_ASSERT(prefs.peekNumber("Hublists", "Total", 0) == 1);
	EXO_ASSERT(!prefs.haveSetting("Hublists", "List 1"));

	EXO_ASSERT(dir->reset());
	EXO_ASSERT(!dir->reset());
	EXO_ASSERT(dir->sources().size() == QuickDC::HubDirectory::defaultSources().size());
	EXO_ASSERT(dir->hubs().empty());
	return 1;
});

/* A fetch on its way for a source taken out is let go, and nothing is told of it. */
EXO_TEST(hubdirectory_a_source_removed_mid_fetch_hears_nothing,
{
	const std::string url = "http://127.0.0.1:1/going.xml";

	Scratch scratch;
	scratch.configure({url});
	QuickDC::Preferences prefs(scratch.conf());
	Told told;
	auto dir = directory(prefs, scratch, told);

	dir->refresh(true, NOW);
	EXO_ASSERT(dir->remove(url));
	const int after = told.sources;

	pump([] { return false; }, 300);
	EXO_ASSERT(told.sources == after);
	EXO_ASSERT(dir->sources().empty());
	return 1;
});
