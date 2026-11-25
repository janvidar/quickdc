/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Pictures in chat: which links in a message are files, which of them are
 * pictures to show, and whether one is fetched before anybody asks.
 */

#include "quickdc.h"
#include "chat/chatmedia.h"
#include "config/preferences.h"
#include "config/settings.h"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

const std::string ROOT = "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A";
const std::string OTHER = "AWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7B";

std::string magnet(const std::string& tth, const std::string& name, uint64_t size)
{
	return "magnet:?xt=urn:tree:tiger:" + tth + "&dn=" + name + "&xl=" + std::to_string(size);
}

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_chatmedia_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

}

EXO_TEST(chatmedia_a_magnet_names_a_file_by_its_hash,
{
	const QuickDC::ChatAttachment file = QuickDC::readMagnet(
		"magnet:?xt=urn:tree:tiger:" + ROOT + "&dn=Holiday%20photo.jpg&xl=123456");
	EXO_ASSERT(file.tth == ROOT);
	EXO_ASSERT(file.name == "Holiday photo.jpg");
	EXO_ASSERT(file.size == 123456);
	return 1;
});

EXO_TEST(chatmedia_a_magnet_name_is_never_a_path,
{
	EXO_ASSERT(QuickDC::readMagnet(magnet(ROOT, "..%2F..%2F.bashrc", 1)).name == ".bashrc");
	EXO_ASSERT(QuickDC::readMagnet(magnet(ROOT, "a%5Cb.png", 1)).name == "b.png");
	EXO_ASSERT(QuickDC::readMagnet(magnet(ROOT, "..", 1)).name == ROOT);
	return 1;
});

EXO_TEST(chatmedia_a_hash_of_the_wrong_shape_is_not_taken,
{
	EXO_ASSERT(QuickDC::readMagnet(magnet(ROOT.substr(1), "a.png", 1)).tth.empty());
	EXO_ASSERT(QuickDC::readMagnet(magnet(ROOT.substr(0, 38) + "1", "a.png", 1)).tth.empty());
	EXO_ASSERT(QuickDC::readMagnet("magnet:?xt=urn:sha1:" + ROOT).tth.empty());
	EXO_ASSERT(QuickDC::readMagnet("http://example.org/?xt=urn:tree:tiger:" + ROOT).tth.empty());

	/* Lower case is the same root. */
	std::string lower = ROOT;
	for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	EXO_ASSERT(QuickDC::readMagnet(magnet(lower, "a.png", 1)).tth == ROOT);
	return 1;
});

EXO_TEST(chatmedia_a_picture_is_shown_and_a_link_is_offered,
{
	const auto found = QuickDC::attachmentsIn(
		"look ![pic](" + magnet(ROOT, "pic.png", 10) + ") and [file](" + magnet(OTHER, "file.zip", 20) + ")");
	EXO_ASSERT(found.size() == 2);
	if (found.size() == 2)
	{
		EXO_ASSERT(found[0].tth == ROOT && found[0].inlineMedia && found[0].name == "pic.png");
		EXO_ASSERT(found[1].tth == OTHER && !found[1].inlineMedia && found[1].size == 20);
	}
	return 1;
});

EXO_TEST(chatmedia_code_is_not_a_link,
{
	const std::string picture = "![pic](" + magnet(ROOT, "pic.png", 10) + ")";

	EXO_ASSERT(QuickDC::attachmentsIn("`" + picture + "`").empty());
	EXO_ASSERT(QuickDC::attachmentsIn("``a ` " + picture + "``").empty());
	EXO_ASSERT(QuickDC::attachmentsIn("```\n" + picture + "\n```").empty());
	EXO_ASSERT(QuickDC::attachmentsIn("~~~~\n" + picture + "\n~~~\n" + picture + "\n~~~~").empty());
	EXO_ASSERT(QuickDC::attachmentsIn("text\n\n    " + picture).empty());
	EXO_ASSERT(QuickDC::attachmentsIn("\\" + picture).size() == 1);
	EXO_ASSERT(!QuickDC::attachmentsIn("\\" + picture)[0].inlineMedia);

	/* An indented line inside a paragraph is part of it, and the fence closed lets what follows through. */
	EXO_ASSERT(QuickDC::attachmentsIn("text\n    " + picture).size() == 1);
	EXO_ASSERT(QuickDC::attachmentsIn("```\ncode\n```\n" + picture).size() == 1);

	/* An unclosed backtick is a backtick. */
	EXO_ASSERT(QuickDC::attachmentsIn("a ` b " + picture).size() == 1);
	return 1;
});

EXO_TEST(chatmedia_the_forms_a_link_can_take,
{
	const std::string uri = magnet(ROOT, "pic.png", 10);

	EXO_ASSERT(QuickDC::attachmentsIn("![pic](<" + uri + ">)").size() == 1);
	EXO_ASSERT(QuickDC::attachmentsIn("![pic]( " + uri + " \"a title\" )").size() == 1);
	EXO_ASSERT(QuickDC::attachmentsIn("![a [nested] alt](" + uri + ")").size() == 1);
	EXO_ASSERT(QuickDC::attachmentsIn("> quoted ![pic](" + uri + ")").size() == 1);

	const auto autolink = QuickDC::attachmentsIn("<" + uri + ">");
	EXO_ASSERT(autolink.size() == 1 && !autolink[0].inlineMedia);

	/* A picture that is itself a link: both are files the message names. */
	const auto nested = QuickDC::attachmentsIn("[![pic](" + uri + ")](" + magnet(OTHER, "big.png", 5) + ")");
	EXO_ASSERT(nested.size() == 2);
	if (nested.size() == 2) EXO_ASSERT(!nested[0].inlineMedia && nested[0].tth == OTHER
	                                   && nested[1].inlineMedia && nested[1].tth == ROOT);

	/* Not links: no destination, a space before it, a blank line in the text, another scheme. */
	EXO_ASSERT(QuickDC::attachmentsIn("![pic] (" + uri + ")").empty());
	EXO_ASSERT(QuickDC::attachmentsIn("![pic\n\nmore](" + uri + ")").empty());
	EXO_ASSERT(QuickDC::attachmentsIn("![pic](https://example.org/pic.png)").empty());
	EXO_ASSERT(QuickDC::attachmentsIn("![pic](" + uri).empty());
	return 1;
});

EXO_TEST(chatmedia_what_is_fetched_unasked,
{
	using QuickDC::ChatMediaSkip;
	const uint64_t limit = 2 * 1024 * 1024;

	EXO_ASSERT(QuickDC::chatMediaVerdict(1000, true, limit) == ChatMediaSkip::None);
	EXO_ASSERT(QuickDC::chatMediaVerdict(limit, true, limit) == ChatMediaSkip::None);
	EXO_ASSERT(QuickDC::chatMediaVerdict(limit + 1, true, limit) == ChatMediaSkip::TooLarge);
	EXO_ASSERT(QuickDC::chatMediaVerdict(limit + 1, true, 0) == ChatMediaSkip::None);
	EXO_ASSERT(QuickDC::chatMediaVerdict(0, true, limit) == ChatMediaSkip::Unsized);
	EXO_ASSERT(QuickDC::chatMediaVerdict(1000, false, limit) == ChatMediaSkip::Off);
	return 1;
});

EXO_TEST(chatmedia_a_hub_may_decide_for_itself,
{
	const std::string cfg = scratch("config.conf");
	const std::string ses = scratch("session.conf");
	{
		QuickDC::Preferences config(cfg);
		QuickDC::Preferences session(ses);
		const std::string hub = "adc://hub.example.org:1511";
		const char* key = QuickDC::Settings::ChatMediaFetch.key;

		EXO_ASSERT(!QuickDC::chatMediaFetchedOn(config, session, hub));
		QuickDC::Settings::write(config, QuickDC::Settings::ChatMediaFetch, true);
		EXO_ASSERT(QuickDC::chatMediaFetchedOn(config, session, hub));

		/* Under the hub's address up to its path, however the message spelled it. */
		session.setBool("adc://hub.example.org:1511", key, false);
		EXO_ASSERT(!QuickDC::chatMediaFetchedOn(config, session, hub));
		EXO_ASSERT(!QuickDC::chatMediaFetchedOn(config, session, " adc://hub.example.org:1511/?kp=x"));
		EXO_ASSERT(QuickDC::chatMediaFetchedOn(config, session, "adc://other.example.org:1511"));

		QuickDC::Settings::write(config, QuickDC::Settings::ChatMediaFetch, false);
		session.setBool("dchub://nmdc.example.org:411", key, true);
		EXO_ASSERT(QuickDC::chatMediaFetchedOn(config, session, "nmdc.example.org"));
	}
	std::filesystem::remove(cfg);
	std::filesystem::remove(ses);
	return 1;
});

EXO_TEST(chatmedia_a_pasted_picture_is_named_for_when,
{
	const std::string name = QuickDC::pastedName(0, "PNG");
	EXO_ASSERT(name.rfind("pasted-", 0) == 0);
	EXO_ASSERT(name.size() == std::string("pasted-19700101-000000.png").size());
	EXO_ASSERT(name.substr(name.size() - 4) == ".png");

	EXO_ASSERT(QuickDC::pastedName(0, "../x").substr(name.size() - 4) == ".x");
	EXO_ASSERT(QuickDC::pastedName(0, "").substr(name.size() - 4) == ".png");
	return 1;
});

EXO_TEST(chatmedia_only_old_pasted_pictures_are_swept,
{
	namespace fs = std::filesystem;
	const fs::path folder = scratch("media");
	fs::create_directories(folder);

	const auto write = [&folder](const std::string& name, int daysOld) {
		std::ofstream(folder / name) << name;
		fs::last_write_time(folder / name, fs::file_time_type::clock::now() - std::chrono::hours(24 * daysOld));
	};
	write("pasted-old.png", 10);
	write("pasted-new.png", 1);
	write("mine.png", 10);

	const std::time_t now = std::time(nullptr);
	EXO_ASSERT(QuickDC::sweepPasted(folder.string(), 0, now) == 0);
	EXO_ASSERT(QuickDC::sweepPasted(folder.string(), 5, now) == 1);
	EXO_ASSERT(!fs::exists(folder / "pasted-old.png"));
	EXO_ASSERT(fs::exists(folder / "pasted-new.png"));
	EXO_ASSERT(fs::exists(folder / "mine.png"));
	EXO_ASSERT(QuickDC::sweepPasted((folder / "absent").string(), 5, now) == 0);

	fs::remove_all(folder);
	return 1;
});
