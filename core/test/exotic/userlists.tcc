/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The hubs kept, the hubs visited and what finished downloading: read from the
 * file in the shape the interface stored them in before the core kept them, and
 * written back in it.
 */

#include "quickdc.h"
#include "api/huburl.h"
#include "config/preferences.h"
#include "config/userlists.h"

#include <samurai/io/file.h>

#include <stdio.h>
#include <string>
#include <unistd.h>

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_userlists_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/* A configuration file that is gone when the case is. */
class Conf {
	public:
		explicit Conf(const std::string& content = std::string())
			: path(scratch("lists.conf"))
		{
			if (content.empty()) return;
			FILE* fh = fopen(path.c_str(), "wb");
			if (!fh) return;
			fwrite(content.data(), 1, content.size(), fh);
			fclose(fh);
		}

		~Conf() { Samurai::IO::File(path).remove(); }

		Conf(const Conf&) = delete;
		Conf& operator=(const Conf&) = delete;

		std::string path;
};

}

EXO_TEST(userlists_a_hub_address_has_one_spelling,
{
	EXO_ASSERT(QuickDC::canonicalHubUrl(" example.org ") == "dchub://example.org:411");
	EXO_ASSERT(QuickDC::canonicalHubUrl("adc://example.org:7817") == "adc://example.org:7817");
	EXO_ASSERT(QuickDC::canonicalHubUrl("adcs://example.org") == "adcs://example.org:411");
	EXO_ASSERT(QuickDC::canonicalHubUrl("adc://example.org/path") == "adc://example.org:411/path");
	EXO_ASSERT(QuickDC::canonicalHubUrl("dchub://[::1]") == "dchub://[::1]:411");
	EXO_ASSERT(QuickDC::canonicalHubUrl("dchub://[::1]:412") == "dchub://[::1]:412");
	EXO_ASSERT(QuickDC::canonicalHubUrl("   ").empty());
	return 1;
});

/* A list from before addresses were canonical is rewritten once, two spellings of a hub as one. */
EXO_TEST(userlists_favourites_are_read_canonical_and_written_back,
{
	Conf conf("[Favourites]\nTotal=4\nHub 0=example.org\nHub 1=dchub://example.org:411\n"
	          "Hub 2=\nHub 3=adcs://other.org:412\n");
	{
		QuickDC::Preferences prefs(conf.path);
		QuickDC::FavouriteHubs kept(prefs);

		EXO_ASSERT_EQ_UINT(kept.urls().size(), 2u);
		EXO_ASSERT(kept.urls()[0] == "dchub://example.org:411");
		EXO_ASSERT(kept.urls()[1] == "adcs://other.org:412");
		EXO_ASSERT(prefs.peekNumber("Favourites", "Total", 0) == 2);
		EXO_ASSERT(prefs.peekString("Favourites", "Hub 1", "") == "adcs://other.org:412");
		EXO_ASSERT(!prefs.haveSetting("Favourites", "Hub 2"));
		EXO_ASSERT(!prefs.haveSetting("Favourites", "Hub 3"));
	}

	/* Saved as it was rewritten, so a second reading changes nothing. */
	QuickDC::Preferences again(conf.path);
	QuickDC::FavouriteHubs kept(again);
	EXO_ASSERT_EQ_UINT(kept.urls().size(), 2u);
	return 1;
});

EXO_TEST(userlists_a_favourite_is_kept_once_however_it_is_spelled,
{
	Conf conf;
	QuickDC::Preferences prefs(conf.path);
	QuickDC::FavouriteHubs kept(prefs);

	EXO_ASSERT(kept.add("hub.example.org"));
	EXO_ASSERT(!kept.add("dchub://hub.example.org:411"));
	EXO_ASSERT(kept.add("adc://two.example.org:1511"));
	EXO_ASSERT(kept.contains(" hub.example.org "));

	EXO_ASSERT(kept.remove("dchub://hub.example.org"));
	EXO_ASSERT(!kept.remove("dchub://hub.example.org"));
	EXO_ASSERT_EQ_UINT(kept.urls().size(), 1u);
	EXO_ASSERT(prefs.peekString("Favourites", "Hub 0", "") == "adc://two.example.org:1511");
	EXO_ASSERT(!prefs.haveSetting("Favourites", "Hub 1"));
	return 1;
});

/* Newest first, a hub visited again moves to the front keeping its name, and the list is bounded. */
EXO_TEST(userlists_recent_hubs_keep_the_newest_visit_first,
{
	Conf conf("[Recent hubs]\nTotal=3\n"
	          "Hub 0=100|adc://old.org:411|Old | Hub\n"
	          "Hub 1=300|adc://new.org:411|New\n"
	          "Hub 2=200|adc://old.org:411|Repeat\n");

	QuickDC::Preferences prefs(conf.path);
	QuickDC::RecentHubs recent(prefs);

	EXO_ASSERT_EQ_UINT(recent.entries().size(), 2u);
	EXO_ASSERT(recent.entries()[0].url == "adc://new.org:411");
	EXO_ASSERT(recent.entries()[1].name == "Old | Hub");

	recent.visited("adc://old.org", 400);
	EXO_ASSERT(recent.entries()[0].url == "adc://old.org:411");
	EXO_ASSERT(recent.entries()[0].name == "Old | Hub");
	EXO_ASSERT(recent.entries()[0].visited == 400);
	EXO_ASSERT(prefs.peekString("Recent hubs", "Hub 0", "") == "400|adc://old.org:411|Old | Hub");

	/* A name is not a visit. */
	EXO_ASSERT(recent.named("adc://new.org:411", "Renamed"));
	EXO_ASSERT(!recent.named("adc://new.org:411", "Renamed"));
	EXO_ASSERT(recent.entries()[0].url == "adc://old.org:411");
	EXO_ASSERT(recent.entries()[1].name == "Renamed");

	for (int n = 0; n < QuickDC::RecentHubs::LIMIT + 5; ++n)
		recent.visited("adc://hub" + std::to_string(n) + ".org", 1000 + n);
	EXO_ASSERT_EQ_UINT(recent.entries().size(), static_cast<size_t>(QuickDC::RecentHubs::LIMIT));
	EXO_ASSERT(prefs.peekNumber("Recent hubs", "Total", 0) == QuickDC::RecentHubs::LIMIT);

	EXO_ASSERT(recent.remove("adc://hub40.org:411") == false);
	EXO_ASSERT(recent.remove("adc://hub36.org"));
	EXO_ASSERT(recent.clear());
	EXO_ASSERT(!recent.clear());
	EXO_ASSERT(prefs.peekNumber("Recent hubs", "Total", -1) == 0);
	EXO_ASSERT(!prefs.haveSetting("Recent hubs", "Hub 0"));
	return 1;
});

/* One completion told twice is one entry; the same file fetched again later is two. */
EXO_TEST(userlists_completed_downloads_are_recorded_once_each,
{
	Conf conf("[Completed]\nTotal=2\n"
	          "Entry 0=100|10|AAAA|/downloads/a|b.bin\n"
	          "Entry 1=200|20|BBBB|/downloads/c.bin\n");

	QuickDC::Preferences prefs(conf.path);
	QuickDC::CompletedDownloads done(prefs);

	EXO_ASSERT_EQ_UINT(done.entries().size(), 2u);
	EXO_ASSERT(done.entries()[0].filename == "/downloads/c.bin");
	EXO_ASSERT(done.entries()[1].filename == "/downloads/a|b.bin");
	EXO_ASSERT(done.entries()[1].size == 10);

	EXO_ASSERT(done.add({"/downloads/c.bin", "BBBB", 20, 203}) == false);
	EXO_ASSERT(done.add({"/downloads/c.bin", "BBBB", 20, 300}));
	EXO_ASSERT(!done.add({"", "CCCC", 1, 400}));
	EXO_ASSERT_EQ_UINT(done.entries().size(), 3u);
	EXO_ASSERT(prefs.peekString("Completed", "Entry 0", "") == "300|20|BBBB|/downloads/c.bin");

	EXO_ASSERT(done.remove("/downloads/a|b.bin"));
	EXO_ASSERT_EQ_UINT(done.entries().size(), 2u);
	EXO_ASSERT(!prefs.haveSetting("Completed", "Entry 2"));

	EXO_ASSERT(done.clear());
	EXO_ASSERT(prefs.peekNumber("Completed", "Total", -1) == 0);
	return 1;
});

EXO_TEST(userlists_completed_downloads_are_bounded,
{
	Conf conf;
	QuickDC::Preferences prefs(conf.path);
	QuickDC::CompletedDownloads done(prefs);

	for (int n = 0; n < QuickDC::CompletedDownloads::LIMIT + 3; ++n)
		done.add({"/d/" + std::to_string(n), "", 1, static_cast<uint64_t>(1000 + n)});

	EXO_ASSERT_EQ_UINT(done.entries().size(), static_cast<size_t>(QuickDC::CompletedDownloads::LIMIT));
	EXO_ASSERT(done.entries()[0].filename == "/d/" + std::to_string(QuickDC::CompletedDownloads::LIMIT + 2));
	return 1;
});
