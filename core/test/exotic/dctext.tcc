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
#ifndef _WIN32
#include <sys/resource.h>
#endif

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
#ifdef _WIN32
	return 0;
#else
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#ifdef __APPLE__
	return (size_t) usage.ru_maxrss;
#else
	return (size_t) usage.ru_maxrss * 1024;
#endif
#endif
}

/*
 * AddressSanitizer's redzones and quarantine grow the resident set by far more
 * than the threshold the measurement below allows, so under it the figure is
 * the allocator's and says nothing about the code. The loop still runs - where
 * a sanitizer is in play its own leak detection is the better check anyway.
 */
#if defined(__SANITIZE_ADDRESS__) || defined(_WIN32)
#define MEASURE_RESIDENT_GROWTH 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MEASURE_RESIDENT_GROWTH 0
#endif
#endif

#ifndef MEASURE_RESIDENT_GROWTH
#define MEASURE_RESIDENT_GROWTH 1
#endif

/* Text with none of the protocol's punctuation in it is left alone. */
EXO_TEST(dctext_ordinary_text_is_not_changed,
{
		EXO_ASSERT(escape("") == "");
		EXO_ASSERT(escape("hello there") == "hello there");
		EXO_ASSERT(escape("<nick> said something") == "<nick> said something");
	
	return 1;
});

/*
	 * The three that have to be escaped, one at a time. The replacements were
	 * run one after another with the ampersand last, and find() restarts at the
	 * beginning of the string - so the ampersand pass kept finding the one it
	 * had just written and never stopped.
	 */
EXO_TEST(dctext_each_of_the_three_characters_is_escaped,
{
		EXO_ASSERT(escape("|") == "&#124;");
		EXO_ASSERT(escape("$") == "&#36;");
		EXO_ASSERT(escape("&") == "&amp;");
	
	return 1;
});

/* And the escape it produces is not escaped again on the way past. */
EXO_TEST(dctext_an_escape_is_not_escaped_a_second_time,
{
		EXO_ASSERT(escape("a|b") == "a&#124;b");
		EXO_ASSERT(escape("a$b") == "a&#36;b");
	
	return 1;
});

/* All of them at once, and more than one of each. */
EXO_TEST(dctext_several_at_once_are_each_escaped_once,
{
		EXO_ASSERT(escape("a|b$c&d") == "a&#124;b&#36;c&amp;d");
		EXO_ASSERT(escape("||") == "&#124;&#124;");
		EXO_ASSERT(escape("&&") == "&amp;&amp;");
	
	return 1;
});

/*
	 * The property the protocol depends on: nothing that comes out may end a
	 * command or start a field, whatever went in.
	 */
EXO_TEST(dctext_nothing_escaped_can_end_a_command_or_start_a_field,
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
				const std::string why = std::string("\"") + nasty[n]
					+ "\" escaped to \"" + out + "\"";
				EXO_FAIL(why.c_str());
			}
		}
	
	return 1;
});

/* Coming back the other way. */
EXO_TEST(dctext_the_three_escapes_are_decoded,
{
		EXO_ASSERT(unescape("") == "");
		EXO_ASSERT(unescape("hello") == "hello");
		EXO_ASSERT(unescape("&#124;") == "|");
		EXO_ASSERT(unescape("&#36;") == "$");
		EXO_ASSERT(unescape("&amp;") == "&");
		EXO_ASSERT(unescape("a&#124;b&#36;c&amp;d") == "a|b$c&d");
	
	return 1;
});

/* Something that only looks like an escape is left as it is. */
EXO_TEST(dctext_a_partial_escape_is_left_alone,
{
		EXO_ASSERT(unescape("&#12") == "&#12");
		EXO_ASSERT(unescape("&amp") == "&amp");
		EXO_ASSERT(unescape("&") == "&");
		EXO_ASSERT(unescape("100% & more") == "100% & more");
	
	return 1;
});

/*
	 * An escaped ampersand is decoded last, so text that was itself an escape
	 * before it was escaped comes back as that text rather than as what it
	 * would have meant.
	 */
EXO_TEST(dctext_an_escaped_escape_decodes_to_the_text_not_the_character,
{
		EXO_ASSERT(unescape("&amp;#124;") == "&#124;");
	
	return 1;
});

/* Which is what makes the round trip an identity. */
EXO_TEST(dctext_escaping_and_unescaping_is_a_round_trip,
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
				const std::string why = std::string("\"") + originals[n]
					+ "\" came back as \"" + back + "\"";
				EXO_FAIL(why.c_str());
			}
		}
	
	return 1;
});

/* A tag as every other client sends it. */
EXO_TEST(dctext_a_dc_tag_parses,
{
		DC::Tag tag("<++ V:0.868,M:A,H:1/0/0,S:2>");
		EXO_ASSERT(tag.isOK());

		/* "++" is how DC++ names itself. */
		EXO_ASSERT(tag.getUserAgent() && std::string(tag.getUserAgent()) == "DC++ 0.868");
		EXO_ASSERT(tag.getSlots() == 2);
		EXO_ASSERT(tag.haveMode());
		EXO_ASSERT(tag.isActiveMode());
		EXO_ASSERT(!tag.isPassiveMode());

		int op = -1, reg = -1, none = -1;
		EXO_ASSERT(tag.getHubCount(op, reg, none));
		EXO_ASSERT(op == 1 && reg == 0 && none == 0);
	
	return 1;
});

/* And one from something else, passive, on several hubs. */
EXO_TEST(dctext_another_client_s_tag_parses,
{
		DC::Tag tag("<QuickDC V:0.5,M:P,H:2/1/3,S:5>");
		EXO_ASSERT(tag.isOK());
		EXO_ASSERT(std::string(tag.getUserAgent()) == "QuickDC 0.5");
		EXO_ASSERT(tag.getSlots() == 5);
		EXO_ASSERT(tag.isPassiveMode());
		EXO_ASSERT(!tag.isActiveMode());

		int op = 0, reg = 0, none = 0;
		EXO_ASSERT(tag.getHubCount(op, reg, none));
		EXO_ASSERT(op == 2 && reg == 1 && none == 3);
	
	return 1;
});

/* The user agent is worked out once and kept. */
EXO_TEST(dctext_the_user_agent_is_only_built_once,
{
		DC::Tag tag("<client V:1.0,M:A,H:1/0/0,S:1>");
		const char* first = tag.getUserAgent();
		const char* again = tag.getUserAgent();
		EXO_ASSERT(first && again && first == again);
	
	return 1;
});

/* With no version there is nothing to append. */
EXO_TEST(dctext_a_tag_with_no_version_is_just_the_client_name,
{
		DC::Tag tag("<client M:A,H:1/0/0,S:1>");
		EXO_ASSERT(tag.isOK());
		EXO_ASSERT(tag.getUserAgent() && std::string(tag.getUserAgent()) == "client");
	
	return 1;
});

/* Fields that are not there are absent rather than made up. */
EXO_TEST(dctext_fields_a_tag_does_not_carry_are_absent,
{
		DC::Tag tag("<client V:1.0>");
		EXO_ASSERT(tag.isOK());
		EXO_ASSERT(!tag.haveMode());
		EXO_ASSERT(!tag.isActiveMode());
		EXO_ASSERT(!tag.isPassiveMode());
		EXO_ASSERT(tag.getSlots() == 0);

		int op = -1, reg = -1, none = -1;
		EXO_ASSERT(!tag.getHubCount(op, reg, none));
	
	return 1;
});

/* A mode nobody defines is neither of the two. */
EXO_TEST(dctext_an_unknown_mode_is_neither_active_nor_passive,
{
		DC::Tag tag("<client V:1.0,M:X,H:1/0/0,S:1>");
		EXO_ASSERT(!tag.haveMode());
		EXO_ASSERT(!tag.isActiveMode());
		EXO_ASSERT(!tag.isPassiveMode());
	
	return 1;
});

/* A slot count that is not a number, or is negative. */
EXO_TEST(dctext_a_slot_count_that_is_not_one_reads_as_none,
{
		DC::Tag a("<client V:1,M:A,H:1/0/0,S:nonsense>");
		EXO_ASSERT(a.getSlots() == 0);

		DC::Tag b("<client V:1,M:A,H:1/0/0,S:-4>");
		EXO_ASSERT(b.getSlots() == 0);
	
	return 1;
});

/* A hub count that is not three numbers. */
EXO_TEST(dctext_a_hub_count_that_is_not_three_numbers_is_refused,
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
				const std::string why =
					std::string("accepted a hub count in \"") + bad[n] + "\"";
				EXO_FAIL(why.c_str());
			}
		}
	
	return 1;
});

/* Parameters nobody knows are ignored, and the rest still read. */
EXO_TEST(dctext_unknown_parameters_are_ignored,
{
		DC::Tag tag("<client V:1.0,Z:something,M:A,QQ:other,S:3>");
		EXO_ASSERT(tag.isOK());
		EXO_ASSERT(std::string(tag.getUserAgent()) == "client 1.0");
		EXO_ASSERT(tag.isActiveMode());
		EXO_ASSERT(tag.getSlots() == 3);
	
	return 1;
});

/* A parameter given twice keeps the last one. */
EXO_TEST(dctext_a_parameter_given_twice_keeps_the_last,
{
		DC::Tag tag("<client V:1.0,S:1,S:9,M:A>");
		EXO_ASSERT(tag.getSlots() == 9);
		EXO_ASSERT(tag.isActiveMode());
	
	return 1;
});

/* Strings that are not tags at all. */
EXO_TEST(dctext_a_string_that_is_not_a_tag_is_refused,
{

		const char* bad[] = { "", "<", "<>", "<noseparator>", "novelope", 0 };

		for (size_t n = 0; bad[n]; n++)
		{
			DC::Tag tag(bad[n]);
			if (tag.isOK())
			{
				const std::string why =
					std::string("accepted \"") + bad[n] + "\" as a tag";
				EXO_FAIL(why.c_str());
			}
		}
	
	return 1;
});

/*
	 * A hub count that cannot be read duplicates the field to cut it up and
	 * never freed the copy, so every user on the hub whose tag is malformed
	 * leaks it again each time their entry is looked at.
	 */
EXO_TEST(dctext_a_hub_count_that_cannot_be_read_frees_its_copy_grew,
{

		const std::string huge(100000, '9');
		const std::string line = "<client V:1,H:" + huge + ",S:1>";

		DC::Tag tag(line.c_str());

		const size_t before = peakBytes();
		if (!before) EXO_SKIP("this system does not say how much memory the process has used");

		for (int n = 0; n < 4000; n++)
		{
			int op = 0, reg = 0, none = 0;
			if (tag.getHubCount(op, reg, none))
				EXO_FAIL("a hub count of 100000 digits was accepted");
		}

		const size_t after = peakBytes();
		const size_t grew = (after > before) ? (after - before) : 0;

		if (MEASURE_RESIDENT_GROWTH)
		{
			EXO_ASSERT(grew < 50u * 1024u * 1024u);
		}
		else
		{
			EXO_SKIP("resident growth is not measurable under a sanitizer");
		}
	
	return 1;
});
