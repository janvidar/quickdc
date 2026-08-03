/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The two pieces of NMDC text handling.
 *
 * TextEscape is what keeps the protocol's own punctuation out of the text we
 * send: NMDC ends every command with '|' and separates its fields with '$', so
 * a message carrying either of those unescaped does not arrive as a message.
 * Everything typed into a chat window, and the nick and description sent at
 * login, goes through it.
 *
 * Tag is the other direction - the "<client V:1,M:A,H:1/0/0,S:2>" that every
 * other user's $MyINFO carries, which is to say a string from anyone on the
 * hub.
 */

#include "quickdc.h"
#include "network/dc/textescape.h"
#include "network/dc/tag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/resource.h>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static std::string escape(const std::string& in)
{
	return DC::TextEscape::escape(in);
}

/* unescape() works in place, so it needs a copy it is allowed to chew on. */
static std::string unescape(const std::string& in)
{
	char* copy = strdup(in.c_str());
	const std::string out(DC::TextEscape::unescape(copy));
	free(copy);
	return out;
}

/* Peak resident bytes, for the leak that cannot be seen any other way. */
static size_t peakBytes()
{
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#ifdef __APPLE__
	return (size_t) usage.ru_maxrss;
#else
	return (size_t) usage.ru_maxrss * 1024;
#endif
}

/*
 * AddressSanitizer's redzones and quarantine grow the resident set by far more
 * than the threshold the measurement below allows, so under it the figure is
 * the allocator's and says nothing about the code. The loop still runs - where
 * a sanitizer is in play its own leak detection is the better check anyway.
 */
#if defined(__SANITIZE_ADDRESS__)
#define MEASURE_RESIDENT_GROWTH 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MEASURE_RESIDENT_GROWTH 0
#endif
#endif

#ifndef MEASURE_RESIDENT_GROWTH
#define MEASURE_RESIDENT_GROWTH 1
#endif

int main()
{
	setbuf(stdout, 0);

	/* Text with none of the protocol's punctuation in it is left alone. */
	{
		CHECK(escape("") == "");
		CHECK(escape("hello there") == "hello there");
		CHECK(escape("<nick> said something") == "<nick> said something");
		printf("  ok ordinary text is not changed\n");
	}

	/*
	 * The three that have to be escaped, one at a time. The replacements were
	 * run one after another with the ampersand last, and find() restarts at the
	 * beginning of the string - so the ampersand pass kept finding the one it
	 * had just written and never stopped.
	 */
	{
		CHECK(escape("|") == "&#124;");
		CHECK(escape("$") == "&#36;");
		CHECK(escape("&") == "&amp;");
		printf("  ok each of the three characters is escaped\n");
	}

	/* And the escape it produces is not escaped again on the way past. */
	{
		CHECK(escape("a|b") == "a&#124;b");
		CHECK(escape("a$b") == "a&#36;b");
		printf("  ok an escape is not escaped a second time\n");
	}

	/* All of them at once, and more than one of each. */
	{
		CHECK(escape("a|b$c&d") == "a&#124;b&#36;c&amp;d");
		CHECK(escape("||") == "&#124;&#124;");
		CHECK(escape("&&") == "&amp;&amp;");
		printf("  ok several at once are each escaped once\n");
	}

	/*
	 * The property the protocol depends on: nothing that comes out may end a
	 * command or start a field, whatever went in.
	 */
	{
		const char* nasty[] = {
			"|$&",
			"$MyINFO $ALL victim |$Quit nobody|",
			"&#124;",
			"&amp;#36;",
			"|||$$$&&&",
			0
		};

		for (size_t n = 0; nasty[n]; n++)
		{
			const std::string out = escape(nasty[n]);
			if (out.find('|') != std::string::npos || out.find('$') != std::string::npos)
			{
				printf("FAIL %s:%d \"%s\" escaped to \"%s\"\n",
					__FILE__, __LINE__, nasty[n], out.c_str());
				failures++;
			}
		}
		printf("  ok nothing escaped can end a command or start a field\n");
	}

	/* Coming back the other way. */
	{
		CHECK(unescape("") == "");
		CHECK(unescape("hello") == "hello");
		CHECK(unescape("&#124;") == "|");
		CHECK(unescape("&#36;") == "$");
		CHECK(unescape("&amp;") == "&");
		CHECK(unescape("a&#124;b&#36;c&amp;d") == "a|b$c&d");
		printf("  ok the three escapes are decoded\n");
	}

	/* Something that only looks like an escape is left as it is. */
	{
		CHECK(unescape("&#12") == "&#12");
		CHECK(unescape("&amp") == "&amp");
		CHECK(unescape("&") == "&");
		CHECK(unescape("100% & more") == "100% & more");
		printf("  ok a partial escape is left alone\n");
	}

	/*
	 * An escaped ampersand is decoded last, so text that was itself an escape
	 * before it was escaped comes back as that text rather than as what it
	 * would have meant.
	 */
	{
		CHECK(unescape("&amp;#124;") == "&#124;");
		printf("  ok an escaped escape decodes to the text, not the character\n");
	}

	/* Which is what makes the round trip an identity. */
	{
		const char* originals[] = {
			"",
			"plain text",
			"|",
			"$",
			"&",
			"a|b$c&d",
			"&#124;",
			"&amp;",
			"&amp;#124;",
			"$MyINFO $ALL somebody |",
			"100% & rising | falling $ steady",
			0
		};

		for (size_t n = 0; originals[n]; n++)
		{
			const std::string back = unescape(escape(originals[n]));
			if (back != originals[n])
			{
				printf("FAIL %s:%d \"%s\" came back as \"%s\"\n",
					__FILE__, __LINE__, originals[n], back.c_str());
				failures++;
			}
		}
		printf("  ok escaping and unescaping is a round trip\n");
	}

	/* A tag as every other client sends it. */
	{
		DC::Tag tag("<++ V:0.868,M:A,H:1/0/0,S:2>");
		CHECK(tag.isOK());

		/* "++" is how DC++ names itself. */
		CHECK(tag.getUserAgent() && std::string(tag.getUserAgent()) == "DC++ 0.868");
		CHECK(tag.getSlots() == 2);
		CHECK(tag.haveMode());
		CHECK(tag.isActiveMode());
		CHECK(!tag.isPassiveMode());

		int op = -1, reg = -1, none = -1;
		CHECK(tag.getHubCount(op, reg, none));
		CHECK(op == 1 && reg == 0 && none == 0);
		printf("  ok a DC++ tag parses: %s\n", tag.getUserAgent());
	}

	/* And one from something else, passive, on several hubs. */
	{
		DC::Tag tag("<QuickDC V:0.5,M:P,H:2/1/3,S:5>");
		CHECK(tag.isOK());
		CHECK(std::string(tag.getUserAgent()) == "QuickDC 0.5");
		CHECK(tag.getSlots() == 5);
		CHECK(tag.isPassiveMode());
		CHECK(!tag.isActiveMode());

		int op = 0, reg = 0, none = 0;
		CHECK(tag.getHubCount(op, reg, none));
		CHECK(op == 2 && reg == 1 && none == 3);
		printf("  ok another client's tag parses\n");
	}

	/* The user agent is worked out once and kept. */
	{
		DC::Tag tag("<client V:1.0,M:A,H:1/0/0,S:1>");
		const char* first = tag.getUserAgent();
		const char* again = tag.getUserAgent();
		CHECK(first && again && first == again);
		printf("  ok the user agent is only built once\n");
	}

	/* With no version there is nothing to append. */
	{
		DC::Tag tag("<client M:A,H:1/0/0,S:1>");
		CHECK(tag.isOK());
		CHECK(tag.getUserAgent() && std::string(tag.getUserAgent()) == "client");
		printf("  ok a tag with no version is just the client name\n");
	}

	/* Fields that are not there are absent rather than made up. */
	{
		DC::Tag tag("<client V:1.0>");
		CHECK(tag.isOK());
		CHECK(!tag.haveMode());
		CHECK(!tag.isActiveMode());
		CHECK(!tag.isPassiveMode());
		CHECK(tag.getSlots() == 0);

		int op = -1, reg = -1, none = -1;
		CHECK(!tag.getHubCount(op, reg, none));
		printf("  ok fields a tag does not carry are absent\n");
	}

	/* A mode nobody defines is neither of the two. */
	{
		DC::Tag tag("<client V:1.0,M:X,H:1/0/0,S:1>");
		CHECK(!tag.haveMode());
		CHECK(!tag.isActiveMode());
		CHECK(!tag.isPassiveMode());
		printf("  ok an unknown mode is neither active nor passive\n");
	}

	/* A slot count that is not a number, or is negative. */
	{
		DC::Tag a("<client V:1,M:A,H:1/0/0,S:nonsense>");
		CHECK(a.getSlots() == 0);

		DC::Tag b("<client V:1,M:A,H:1/0/0,S:-4>");
		CHECK(b.getSlots() == 0);
		printf("  ok a slot count that is not one reads as none\n");
	}

	/* A hub count that is not three numbers. */
	{
		const char* bad[] = {
			"<client V:1,H:1>",
			"<client V:1,H:1/2>",
			"<client V:1,H:nonsense>",
			"<client V:1,H:>",
			0
		};

		for (size_t n = 0; bad[n]; n++)
		{
			DC::Tag tag(bad[n]);
			int op = -1, reg = -1, none = -1;
			if (tag.getHubCount(op, reg, none))
			{
				printf("FAIL %s:%d accepted hub count in \"%s\"\n",
					__FILE__, __LINE__, bad[n]);
				failures++;
			}
		}
		printf("  ok a hub count that is not three numbers is refused\n");
	}

	/* Parameters nobody knows are ignored, and the rest still read. */
	{
		DC::Tag tag("<client V:1.0,Z:something,M:A,QQ:other,S:3>");
		CHECK(tag.isOK());
		CHECK(std::string(tag.getUserAgent()) == "client 1.0");
		CHECK(tag.isActiveMode());
		CHECK(tag.getSlots() == 3);
		printf("  ok unknown parameters are ignored\n");
	}

	/* A parameter given twice keeps the last one. */
	{
		DC::Tag tag("<client V:1.0,S:1,S:9,M:A>");
		CHECK(tag.getSlots() == 9);
		CHECK(tag.isActiveMode());
		printf("  ok a parameter given twice keeps the last\n");
	}

	/* Strings that are not tags at all. */
	{
		const char* bad[] = { "", "<", "<>", "<noseparator>", "novelope", 0 };

		for (size_t n = 0; bad[n]; n++)
		{
			DC::Tag tag(bad[n]);
			if (tag.isOK())
			{
				printf("FAIL %s:%d accepted \"%s\" as a tag\n",
					__FILE__, __LINE__, bad[n]);
				failures++;
			}
		}
		printf("  ok a string that is not a tag is refused\n");
	}

	/*
	 * A hub count that cannot be read duplicates the field to cut it up and
	 * never freed the copy, so every user on the hub whose tag is malformed
	 * leaks it again each time their entry is looked at.
	 */
	{
		const std::string huge(100000, '9');
		const std::string line = "<client V:1,H:" + huge + ",S:1>";

		DC::Tag tag(line.c_str());

		const size_t before = peakBytes();
		CHECK(before > 0);

		for (int n = 0; n < 4000; n++)
		{
			int op = 0, reg = 0, none = 0;
			if (tag.getHubCount(op, reg, none))
			{
				printf("FAIL %s:%d a hub count of digits was accepted\n",
					__FILE__, __LINE__);
				failures++;
				break;
			}
		}

		const size_t after = peakBytes();
		const size_t grew = (after > before) ? (after - before) : 0;

		if (MEASURE_RESIDENT_GROWTH)
		{
			CHECK(grew < 50u * 1024u * 1024u);
			printf("  ok a hub count that cannot be read frees its copy (grew %zu KB)\n",
				grew / 1024);
		}
		else
		{
			printf("  skip a hub count that cannot be read frees its copy"
				" (not measurable under a sanitizer)\n");
		}
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
