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
#include <unistd.h>
#include <vector>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

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

int main()
{
	setbuf(stdout, 0);

	/* A file that is not there is an empty set of preferences, not a failure. */
	{
		Conf conf;
		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		CHECK(!prefs.haveSetting("Nickname"));
		CHECK(std::string(prefs.getString("Nickname", "anonymous")) == "anonymous");
		printf("  ok a config file that is not there reads as empty\n");
	}

	/* Sections and settings. */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
			"[User]\n"
			"Nickname=bob\n"
			"Description=a user\n"
			"\n"
			"[Network]\n"
			"Port=1511\n"));

		QuickDC::Preferences prefs(conf.path);

		prefs.setGroup("User");
		CHECK(std::string(prefs.getString("Nickname")) == "bob");
		CHECK(std::string(prefs.getString("Description")) == "a user");

		prefs.setGroup("Network");
		CHECK(prefs.getNumber("Port") == 1511);

		/* A setting belongs to its own section. */
		CHECK(!prefs.haveSetting("Nickname"));
		printf("  ok settings are read into the section they are in\n");
	}

	/* Comments and blank lines are not settings. */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
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
		CHECK(std::string(prefs.getString("Nickname")) == "bob");
		printf("  ok comments and blank lines are skipped\n");
	}

	/* Whitespace around the name and the value is not part of either. */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
			"[User]\n"
			"  Nickname  =  bob  \n"
			"\tDescription\t=\ta user\t\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		CHECK(std::string(prefs.getString("Nickname")) == "bob");
		CHECK(std::string(prefs.getString("Description")) == "a user");
		printf("  ok whitespace around a setting is not part of it\n");
	}

	/* A value may contain anything that is not a newline. */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
			"[Shares]\n"
			"Share 0=music:/home/bob/My Music\n"
			"Equals=a=b=c\n"
			"Semicolon=rock;pop\n"
			"Brackets=[not a section]\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("Shares");
		CHECK(std::string(prefs.getString("Share 0")) == "music:/home/bob/My Music");
		CHECK(std::string(prefs.getString("Equals")) == "a=b=c");
		CHECK(std::string(prefs.getString("Semicolon")) == "rock;pop");
		CHECK(std::string(prefs.getString("Brackets")) == "[not a section]");
		printf("  ok a value keeps everything that is not a newline\n");
	}

	/*
	 * A file somebody edited by hand and saved without a final newline. The
	 * read stopped at the last one it found, so the last setting in the file -
	 * which is where an edit usually is - was silently dropped.
	 */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
			"[User]\n"
			"Nickname=bob\n"
			"Description=edited by hand"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		CHECK(std::string(prefs.getString("Nickname")) == "bob");
		CHECK(prefs.haveSetting("Description"));
		CHECK(std::string(prefs.getString("Description")) == "edited by hand");
		printf("  ok a file with no final newline keeps its last setting\n");
	}

	/* What we write, we read. */
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
			CHECK(prefs.write());
		}

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		CHECK(std::string(prefs.getString("Nickname")) == "bob");
		CHECK(std::string(prefs.getString("Description")) == "a user with spaces");
		CHECK(prefs.getNumber("Slots") == 7);
		CHECK(prefs.getBool("Away"));
		CHECK(!prefs.getBool("Hidden"));

		prefs.setGroup("Network");
		CHECK(prefs.getNumber("Port") == 1511);
		printf("  ok what is written is read back\n");
	}

	/* Including on the way out of scope, which is when it usually happens. */
	{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			prefs.setString("Nickname", "written on the way out");
		}

		CHECK(valueOf(conf.path, "User", "Nickname") == "written on the way out");
		printf("  ok preferences are written when they go out of scope\n");
	}

	/* A password is a setting like any other, and has to survive as typed. */
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
				printf("FAIL %s:%d \"%s\" came back as \"%s\"\n",
					__FILE__, __LINE__, passwords[n], got.c_str());
				failures++;
			}
		}
		printf("  ok a password survives being written and read\n");
	}

	/* A share list, which is the other thing nobody wants to type again. */
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
		CHECK(prefs.getNumber("Total shares") == 3);
		CHECK(std::string(prefs.getString("Share 0")) == "music:/home/bob/Music");
		CHECK(std::string(prefs.getString("Share 1")) == "films:/mnt/big disk/films");
		CHECK(std::string(prefs.getString("Share 2")) == "misc:/tmp");
		printf("  ok a share list survives the round trip\n");
	}

	/* Rewriting replaces the file rather than adding to it. */
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

		CHECK(readFile(conf.path).size() == once);
		CHECK(valueOf(conf.path, "User", "Nickname") == "bbbbb");
		printf("  ok rewriting replaces the file\n");
	}

	/* A setting that is removed stays removed. */
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
			CHECK(prefs.haveSetting("Temporary"));
			prefs.remove("Temporary");
			CHECK(!prefs.haveSetting("Temporary"));
		}

		CHECK(valueOf(conf.path, "User", "Temporary") == "(absent)");
		CHECK(valueOf(conf.path, "User", "Nickname") == "bob");
		printf("  ok a removed setting does not come back\n");
	}

	/* Asking for something absent records the default it was given. */
	{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			CHECK(std::string(prefs.getString("Nickname", "anonymous")) == "anonymous");
			CHECK(prefs.getNumber("Slots", 3) == 3);
			CHECK(prefs.getBool("Away", true));
		}

		CHECK(valueOf(conf.path, "User", "Nickname") == "anonymous");
		CHECK(valueOf(conf.path, "User", "Slots") == "3");
		CHECK(valueOf(conf.path, "User", "Away") == "true");
		printf("  ok a default that was asked for is remembered\n");
	}

	/* And a stored value is preferred over the default. */
	{
		Conf conf;
		CHECK(writeFile(conf.path, "[User]\nNickname=bob\nSlots=9\nAway=false\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		CHECK(std::string(prefs.getString("Nickname", "anonymous")) == "bob");
		CHECK(prefs.getNumber("Slots", 3) == 9);
		CHECK(!prefs.getBool("Away", true));
		printf("  ok a stored setting wins over the default\n");
	}

	/* Both spellings of true, and everything else meaning false. */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
			"[Flags]\nA=true\nB=1\nC=false\nD=0\nE=yes\nF=\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("Flags");
		CHECK(prefs.getBool("A"));
		CHECK(prefs.getBool("B"));
		CHECK(!prefs.getBool("C"));
		CHECK(!prefs.getBool("D"));
		CHECK(!prefs.getBool("E"));
		CHECK(!prefs.getBool("F"));
		printf("  ok a boolean is true when it says so\n");
	}

	/* A timestamp is kept to the second. */
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
		CHECK(prefs.getTime("Last visit").getInternalData() == when.getInternalData());
		printf("  ok a timestamp survives the round trip\n");
	}

	/* The same key in two sections is two settings. */
	{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("HubA");
			prefs.setString("Password", "one");
			prefs.setGroup("HubB");
			prefs.setString("Password", "two");
		}

		CHECK(valueOf(conf.path, "HubA", "Password") == "one");
		CHECK(valueOf(conf.path, "HubB", "Password") == "two");
		printf("  ok the same key in two sections stays two settings\n");
	}

	/*
	 * A setting before any section header. It went into a group with no name,
	 * which is written out as "[]" - and read back as a syntax error, so the
	 * setting was there once and gone the time after.
	 */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
			"Nickname=bob\n"
			"[User]\n"
			"Description=a user\n"));

		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("Default");
			CHECK(prefs.haveSetting("Nickname"));
			CHECK(std::string(prefs.getString("Nickname")) == "bob");

			/* Touch something so it is written back out. */
			prefs.setGroup("User");
			prefs.setString("Description", "a user");
		}

		/* And it is still there after a write and a read. */
		CHECK(valueOf(conf.path, "Default", "Nickname") == "bob");
		CHECK(valueOf(conf.path, "User", "Description") == "a user");
		printf("  ok a setting before any section keeps its home\n");
	}

	/* Files that are not quite right are read for what they do say. */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
			"[User]\n"
			"this line has no equals sign\n"
			"Nickname=bob\n"
			"=value with no name\n"
			"Description=a user\n"));

		QuickDC::Preferences prefs(conf.path);
		prefs.setGroup("User");
		CHECK(std::string(prefs.getString("Nickname")) == "bob");
		CHECK(std::string(prefs.getString("Description")) == "a user");
		printf("  ok a line that is not a setting does not lose the ones that are\n");
	}

	/* An unterminated section header names the section it spells. */
	{
		Conf conf;
		CHECK(writeFile(conf.path,
			"[User\n"
			"Nickname=bob\n"));

		QuickDC::Preferences prefs(conf.path);

		/* Whatever it decides, it must not invent a section called "Use". */
		prefs.setGroup("Use");
		CHECK(!prefs.haveSetting("Nickname"));
		printf("  ok an unterminated section header does not lose a character\n");
	}

	/* The header we write is comments, and it comes back as nothing. */
	{
		Conf conf;
		{
			QuickDC::Preferences prefs(conf.path);
			prefs.setGroup("User");
			prefs.setString("Nickname", "bob");
		}

		const std::string raw = readFile(conf.path);
		CHECK(raw.find("; QuickDC Preferences") != std::string::npos);
		CHECK(raw.find("[User]") != std::string::npos);
		CHECK(raw.find("Nickname=bob") != std::string::npos);

		/* Every comment is on a line of its own. */
		size_t at = 0;
		while ((at = raw.find(';', at)) != std::string::npos)
		{
			const size_t bol = raw.rfind('\n', at);
			const std::string line = raw.substr(bol == std::string::npos ? 0 : bol + 1,
				raw.find('\n', at) - (bol == std::string::npos ? 0 : bol + 1));

			if (!line.empty() && line[0] != ';')
			{
				printf("FAIL %s:%d a comment is not on a line of its own: \"%s\"\n",
					__FILE__, __LINE__, line.c_str());
				failures++;
				break;
			}
			at++;
		}
		printf("  ok the file we write is one we can read\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
