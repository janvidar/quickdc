/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Everything the user configures lives in two ini files: the shared
 * directories, the hub bookmarks, the nick, and the passwords. Nothing else
 * remembers any of it, so a setting that does not survive the round trip
 * through here is a setting the user has to type in again - and in the case of
 * a hub password, one they may not have written down anywhere else.
 *
 * So most of what follows writes a file, reads it back, and insists on getting
 * out what went in, including through files nobody wrote with this code.
 */

#include "quickdc.h"
#include "config/preferences.h"

#include <samurai/io/file.h>

#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <samurai/posix.h>
#include <vector>

static std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_config_" + std::to_string((int) getpid())
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

/* A config file that removes itself when the case is over. */
class Conf
{
	public:
		explicit Conf(const char* leaf = "config.conf") : path(scratch(leaf)) { }
		~Conf() { Samurai::IO::File(path).remove(); }

		Conf(const Conf&) = delete;
		Conf& operator=(const Conf&) = delete;

		std::string path;
};

/* Reads a setting out of a given group of a file, and nothing else. */
static std::string valueOf(const std::string& path, const char* group, const char* key)
{
	QuickDC::Preferences prefs(path);
	prefs.setGroup(group);
	if (!prefs.haveSetting(key)) return std::string("(absent)");
	return std::string(prefs.getString(key));
}

/* A file that is not there is an empty set of preferences, not a failure. */
EXO_TEST(config_a_config_file_that_is_not_there_reads_as_empty,
{
		Conf conf;
		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		EXO_ASSERT(!prefs.haveSetting("Nickname"));
		EXO_ASSERT(std::string(prefs.getString("Nickname", "anonymous")) == "anonymous");
	
	return 1;
});

/* Sections and settings. */
EXO_TEST(config_settings_are_read_into_the_section_they_are_in,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"[User]\n"
			"Nickname=bob\n"
			"Description=a user\n"
			"\n"
			"[Network]\n"
			"Port=1511\n"));

		QuickDC::Preferences prefs(conf.path);

		prefs.setGroup("User");
		EXO_ASSERT(std::string(prefs.getString("Nickname")) == "bob");
		EXO_ASSERT(std::string(prefs.getString("Description")) == "a user");

		prefs.setGroup("Network");
		EXO_ASSERT(prefs.getNumber("Port") == 1511);

		/* A setting belongs to its own section. */
		EXO_ASSERT(!prefs.haveSetting("Nickname"));
	
	return 1;
});

/* Comments and blank lines are not settings. */
EXO_TEST(config_comments_and_blank_lines_are_skipped,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"; a comment\n"
			"# another one\n"
			"\n"
			"[User]\n"
			"   ; an indented comment\n"
			"\n"
			"Nickname=bob\n"
			"\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		EXO_ASSERT(std::string(prefs.getString("Nickname")) == "bob");
	
	return 1;
});

/* Whitespace around the name and the value is not part of either. */
EXO_TEST(config_whitespace_around_a_setting_is_not_part_of_it,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"[User]\n"
			"  Nickname  =  bob  \n"
			"\tDescription\t=\ta user\t\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		EXO_ASSERT(std::string(prefs.getString("Nickname")) == "bob");
		EXO_ASSERT(std::string(prefs.getString("Description")) == "a user");
	
	return 1;
});

/* A value may contain anything that is not a newline. */
EXO_TEST(config_a_value_keeps_everything_that_is_not_a_newline,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"[Shares]\n"
			"Share 0=music:/home/bob/My Music\n"
			"Equals=a=b=c\n"
			"Semicolon=rock;pop\n"
			"Brackets=[not a section]\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("Shares");
		EXO_ASSERT(std::string(prefs.getString("Share 0")) == "music:/home/bob/My Music");
		EXO_ASSERT(std::string(prefs.getString("Equals")) == "a=b=c");
		EXO_ASSERT(std::string(prefs.getString("Semicolon")) == "rock;pop");
		EXO_ASSERT(std::string(prefs.getString("Brackets")) == "[not a section]");
	
	return 1;
});

/*
	 * A file somebody edited by hand and saved without a final newline. The
	 * read stopped at the last one it found, so the last setting in the file -
	 * which is where an edit usually is - was silently dropped.
	 */
EXO_TEST(config_a_file_with_no_final_newline_keeps_its_last_setting,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"[User]\n"
			"Nickname=bob\n"
			"Description=edited by hand"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		EXO_ASSERT(std::string(prefs.getString("Nickname")) == "bob");
		EXO_ASSERT(prefs.haveSetting("Description"));
		EXO_ASSERT(std::string(prefs.getString("Description")) == "edited by hand");
	
	return 1;
});

/* What we write, we read. */
EXO_TEST(config_what_is_written_is_read_back,
{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			prefs.setString("Nickname", "bob");
			prefs.setString("Description", "a user with spaces");
			prefs.setNumber("Slots", 7);
			prefs.setBool("Away", true);
			prefs.setBool("Hidden", false);

			prefs.setGroup("Network");
			prefs.setNumber("Port", 1511);
			EXO_ASSERT(prefs.write());
		}

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		EXO_ASSERT(std::string(prefs.getString("Nickname")) == "bob");
		EXO_ASSERT(std::string(prefs.getString("Description")) == "a user with spaces");
		EXO_ASSERT(prefs.getNumber("Slots") == 7);
		EXO_ASSERT(prefs.getBool("Away"));
		EXO_ASSERT(!prefs.getBool("Hidden"));

		prefs.setGroup("Network");
		EXO_ASSERT(prefs.getNumber("Port") == 1511);
	
	return 1;
});

/* Including on the way out of scope, which is when it usually happens. */
EXO_TEST(config_preferences_are_written_when_they_go_out_of_scope,
{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			prefs.setString("Nickname", "written on the way out");
		}

		EXO_ASSERT(valueOf(conf.path, "User", "Nickname") == "written on the way out");
	
	return 1;
});

/* A password is a setting like any other, and has to survive as typed. */
EXO_TEST(config_a_password_survives_being_written_and_read,
{

		Conf conf;
		const char* passwords[] = {
			"simple",
			"with spaces in it",
			"punctuation!\"#%&/()=?",
			"equals=sign",
			"semi;colon",
			"[brackets]",
			"#hash",
			";semicolon first",
			0
		};

		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("Hub");
			for (size_t n = 0; passwords[n]; n++)
				prefs.setString("Password " + std::to_string(n), passwords[n]);
		}

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("Hub");
		for (size_t n = 0; passwords[n]; n++)
		{
			const std::string key = "Password " + std::to_string(n);
			const std::string got = prefs.getString(key);
			if (got != passwords[n])
			{
				const std::string why = std::string("\"") + passwords[n]
					+ "\" came back as \"" + got + "\"";
				EXO_FAIL(why.c_str());
			}
		}
	
	return 1;
});

/* A share list, which is the other thing nobody wants to type again. */
EXO_TEST(config_a_share_list_survives_the_round_trip,
{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("Shares");
			prefs.setNumber("Total shares", 3);
			prefs.setString("Share 0", "music:/home/bob/Music");
			prefs.setString("Share 1", "films:/mnt/big disk/films");
			prefs.setString("Share 2", "misc:/tmp");
		}

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("Shares");
		EXO_ASSERT(prefs.getNumber("Total shares") == 3);
		EXO_ASSERT(std::string(prefs.getString("Share 0")) == "music:/home/bob/Music");
		EXO_ASSERT(std::string(prefs.getString("Share 1")) == "films:/mnt/big disk/films");
		EXO_ASSERT(std::string(prefs.getString("Share 2")) == "misc:/tmp");
	
	return 1;
});

/* Rewriting replaces the file rather than adding to it. */
EXO_TEST(config_rewriting_replaces_the_file,
{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			prefs.setString("Nickname", "aaaaa");
		}
		const size_t once = readFile(conf.path).size();

		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			prefs.setString("Nickname", "bbbbb");
		}

		EXO_ASSERT(readFile(conf.path).size() == once);
		EXO_ASSERT(valueOf(conf.path, "User", "Nickname") == "bbbbb");
	
	return 1;
});

/* A setting that is removed stays removed. */
EXO_TEST(config_a_removed_setting_does_not_come_back,
{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			prefs.setString("Nickname", "bob");
			prefs.setString("Temporary", "goes away");
		}

		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			EXO_ASSERT(prefs.haveSetting("Temporary"));
			prefs.remove("Temporary");
			EXO_ASSERT(!prefs.haveSetting("Temporary"));
		}

		EXO_ASSERT(valueOf(conf.path, "User", "Temporary") == "(absent)");
		EXO_ASSERT(valueOf(conf.path, "User", "Nickname") == "bob");
	
	return 1;
});

/* Asking for something absent records the default it was given. */
EXO_TEST(config_a_default_that_was_asked_for_is_remembered,
{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			EXO_ASSERT(std::string(prefs.getString("Nickname", "anonymous")) == "anonymous");
			EXO_ASSERT(prefs.getNumber("Slots", 3) == 3);
			EXO_ASSERT(prefs.getBool("Away", true));
		}

		EXO_ASSERT(valueOf(conf.path, "User", "Nickname") == "anonymous");
		EXO_ASSERT(valueOf(conf.path, "User", "Slots") == "3");
		EXO_ASSERT(valueOf(conf.path, "User", "Away") == "true");
	
	return 1;
});

/* And a stored value is preferred over the default. */
EXO_TEST(config_a_stored_setting_wins_over_the_default,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path, "[User]\nNickname=bob\nSlots=9\nAway=false\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		EXO_ASSERT(std::string(prefs.getString("Nickname", "anonymous")) == "bob");
		EXO_ASSERT(prefs.getNumber("Slots", 3) == 9);
		EXO_ASSERT(!prefs.getBool("Away", true));
	
	return 1;
});

/* Both spellings of true, and everything else meaning false. */
EXO_TEST(config_a_boolean_is_true_when_it_says_so,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"[Flags]\nA=true\nB=1\nC=false\nD=0\nE=yes\nF=\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("Flags");
		EXO_ASSERT(prefs.getBool("A"));
		EXO_ASSERT(prefs.getBool("B"));
		EXO_ASSERT(!prefs.getBool("C"));
		EXO_ASSERT(!prefs.getBool("D"));
		EXO_ASSERT(!prefs.getBool("E"));
		EXO_ASSERT(!prefs.getBool("F"));
	
	return 1;
});

/* A timestamp is kept to the second. */
EXO_TEST(config_a_timestamp_survives_the_round_trip,
{
		Conf conf;
		Samurai::TimeStamp when(1234567890);
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("Hub");
			prefs.setTime("Last visit", when);
		}

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("Hub");
		EXO_ASSERT(prefs.getTime("Last visit").getInternalData() == when.getInternalData());
	
	return 1;
});

/* The same key in two sections is two settings. */
EXO_TEST(config_the_same_key_in_two_sections_stays_two_settings,
{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("HubA");
			prefs.setString("Password", "one");
			prefs.setGroup("HubB");
			prefs.setString("Password", "two");
		}

		EXO_ASSERT(valueOf(conf.path, "HubA", "Password") == "one");
		EXO_ASSERT(valueOf(conf.path, "HubB", "Password") == "two");
	
	return 1;
});

/*
	 * A setting before any section header. It went into a group with no name,
	 * which is written out as "[]" - and read back as a syntax error, so the
	 * setting was there once and gone the time after.
	 */
EXO_TEST(config_a_setting_before_any_section_keeps_its_home,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"Nickname=bob\n"
			"[User]\n"
			"Description=a user\n"));

		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("Default");
			EXO_ASSERT(prefs.haveSetting("Nickname"));
			EXO_ASSERT(std::string(prefs.getString("Nickname")) == "bob");

			/* Touch something so it is written back out. */
			prefs.setGroup("User");
			prefs.setString("Description", "a user");
		}

		/* And it is still there after a write and a read. */
		EXO_ASSERT(valueOf(conf.path, "Default", "Nickname") == "bob");
		EXO_ASSERT(valueOf(conf.path, "User", "Description") == "a user");
	
	return 1;
});

/* Files that are not quite right are read for what they do say. */
EXO_TEST(config_a_line_that_is_not_a_setting_does_not_lose_the,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"[User]\n"
			"this line has no equals sign\n"
			"Nickname=bob\n"
			"=value with no name\n"
			"Description=a user\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		EXO_ASSERT(std::string(prefs.getString("Nickname")) == "bob");
		EXO_ASSERT(std::string(prefs.getString("Description")) == "a user");
	
	return 1;
});

/* An unterminated section header names the section it spells. */
EXO_TEST(config_an_unterminated_section_header_does_not_lose_a_character,
{
		Conf conf;
		EXO_ASSERT(writeFile(conf.path,
			"[User\n"
			"Nickname=bob\n"));

		QuickDC::Preferences prefs(conf.path);

		/* Whatever it decides, it must not invent a section called "Use". */
		prefs.setGroup("Use");
		EXO_ASSERT(!prefs.haveSetting("Nickname"));
	
	return 1;
});

/* The header we write is comments, and it comes back as nothing. */
EXO_TEST(config_the_file_we_write_is_one_we_can_read,
{

		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			prefs.setString("Nickname", "bob");
		}

		const std::string raw = readFile(conf.path);
		EXO_ASSERT(raw.find("; QuickDC Preferences") != std::string::npos);
		EXO_ASSERT(raw.find("[User]") != std::string::npos);
		EXO_ASSERT(raw.find("Nickname=bob") != std::string::npos);

		/* Every comment is on a line of its own. */
		size_t at = 0;
		while ((at = raw.find(';', at)) != std::string::npos)
		{
			const size_t bol = raw.rfind('\n', at);
			const std::string line = raw.substr(bol == std::string::npos ? 0 : bol + 1,
				raw.find('\n', at) - (bol == std::string::npos ? 0 : bol + 1));

			if (!line.empty() && line[0] != ';')
			{
				const std::string why =
					"a comment is not on a line of its own: \"" + line + "\"";
				EXO_FAIL(why.c_str());
			}
			at++;
		}
	
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Being told of changes                                                     */
/* ------------------------------------------------------------------------- */

namespace {

class ChangeRecorder : public QuickDC::PreferencesListener {
	public:
		struct Change {
			std::string group;
			std::string key;
			bool removed;
			std::string value;
		};

		void EventSettingChanged(const QuickDC::Preferences&, const std::string& group,
		                         const std::string& key, const std::string* value) override
		{
			changes.push_back(Change{group, key, value == nullptr, value ? *value : ""});
		}

		std::vector<Change> changes;
};

}

EXO_TEST(config_a_change_is_told_whichever_way_it_is_made,
{
	const std::string path = scratch("told.conf");
	{
		QuickDC::Preferences prefs(path);
		ChangeRecorder recorder;
		QuickDC::Subscription subscription = prefs.subscribe(&recorder);
		const uint64_t before = prefs.revision();

		prefs.setString("Network", "Address", "192.0.2.1");
		prefs.setGroup("Network");
		prefs.setNumber("Port", 1412);
		prefs.setBool("User", "Away", true);

		EXO_ASSERT_EQ_UINT(recorder.changes.size(), 3u);
		EXO_ASSERT(recorder.changes[0].group == "Network" && recorder.changes[0].key == "Address");
		EXO_ASSERT(recorder.changes[0].value == "192.0.2.1");
		EXO_ASSERT(recorder.changes[1].key == "Port" && recorder.changes[1].value == "1412");
		EXO_ASSERT(recorder.changes[2].group == "User" && recorder.changes[2].value == "true");
		EXO_ASSERT_EQ_UINT(prefs.revision() - before, 3u);
	}
	Samurai::IO::File(path).remove();
	return 1;
});

/* Writing what is already there is not a change, and nobody is told of one. */
EXO_TEST(config_setting_a_key_to_what_it_holds_is_not_a_change,
{
	const std::string path = scratch("same.conf");
	{
		QuickDC::Preferences prefs(path);
		prefs.setString("Network", "Address", "192.0.2.1");

		ChangeRecorder recorder;
		QuickDC::Subscription subscription = prefs.subscribe(&recorder);
		const uint64_t before = prefs.revision();

		prefs.setString("Network", "Address", "192.0.2.1");

		EXO_ASSERT(recorder.changes.empty());
		EXO_ASSERT_EQ_UINT(prefs.revision(), before);
	}
	Samurai::IO::File(path).remove();
	return 1;
});

/* A removal is told as one; removing what is not there is not. */
EXO_TEST(config_a_removal_is_told_and_removing_nothing_is_not,
{
	const std::string path = scratch("removed.conf");
	{
		QuickDC::Preferences prefs(path);
		prefs.setString("Network", "Address", "192.0.2.1");

		ChangeRecorder recorder;
		QuickDC::Subscription subscription = prefs.subscribe(&recorder);

		prefs.remove("Network", "Address");
		prefs.remove("Network", "Address");
		prefs.remove("Nowhere", "Nothing");

		EXO_ASSERT_EQ_UINT(recorder.changes.size(), 1u);
		EXO_ASSERT(recorder.changes[0].removed);
		EXO_ASSERT(recorder.changes[0].key == "Address");
	}
	Samurai::IO::File(path).remove();
	return 1;
});

/* A read that fills in a default writes it, and that write is told too. */
EXO_TEST(config_a_default_written_by_a_read_is_told,
{
	const std::string path = scratch("defaulted.conf");
	{
		QuickDC::Preferences prefs(path);
		ChangeRecorder recorder;
		QuickDC::Subscription subscription = prefs.subscribe(&recorder);

		prefs.setGroup("Network");
		EXO_ASSERT_EQ_INT(prefs.getNumber("Port", 1411), 1411);
		EXO_ASSERT_EQ_INT(prefs.getNumber("Port", 1411), 1411);

		EXO_ASSERT_EQ_UINT(recorder.changes.size(), 1u);
		EXO_ASSERT(recorder.changes[0].value == "1411");
	}
	Samurai::IO::File(path).remove();
	return 1;
});
