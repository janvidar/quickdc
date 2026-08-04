/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * ADC::Parser::parse() is the first thing that touches a line from a hub, a
 * peer, or a UDP datagram from anywhere at all, so everything here is somebody
 * else's input. It splits the line into arguments, unescapes each one, and
 * pulls out the source and target IDs the prefix calls for.
 *
 * The other direction - what write() puts on the wire - is adccommand.cpp.
 * What this file adds is the decoding, the round trip between the two, and the
 * parts of a command a parsed line has that a constructed one does not: the
 * feature lists of an F command.
 */

#include "quickdc.h"
#include "network/adc/parser.h"

#include <samurai/io/buffer.h>

#include <stdio.h>
#include <string.h>
#include <string>

/* Parses a literal, with the length taken from the literal rather than a NUL. */
#define PARSE(cmd, literal) ADC::Parser::parse(literal, sizeof(literal) - 1, cmd)

static std::string written(ADC::Command& cmd)
{
	Samurai::IO::Buffer buf;
	cmd.write(&buf);
	return buf.copyRange(0, buf.size());
}

/* Newlines shown as an escape, so a failure prints on one line. */
static std::string shown(const std::string& in)
{
	std::string out;
	for (size_t n = 0; n < in.size(); n++)
	{
		if (in[n] == '\n') out += "\\n";
		else if (!in[n]) out += "\\0";
		else out += in[n];
	}
	return out;
}

/* The arguments left after the prefix has taken the IDs it needs. */
static std::string args(ADC::Command& cmd)
{
	std::string out;
	for (size_t n = 0; n < cmd.countArguments(); n++)
	{
		if (n) out += "|";
		out += cmd.getArgument(n);
	}
	return out;
}

/* Too short to hold a command name. */
EXO_TEST(adcparser_a_line_too_short_to_name_a_command_is_refused,
{
		ADC::Command cmd;
		EXO_ASSERT(!ADC::Parser::parse(0, 10, cmd));
		EXO_ASSERT(!PARSE(cmd, ""));
		EXO_ASSERT(!PARSE(cmd, "I"));
		EXO_ASSERT(!PARSE(cmd, "IIN"));
	
	return 1;
});

/* A command whose prefix needs no ID at all. */
EXO_TEST(adcparser_an_info_command_keeps_all_of_its_arguments,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "ISUP ADBASE ADTIGR"));
		EXO_ASSERT(cmd.getFourcc() == FOURCC('I','S','U','P'));
		EXO_ASSERT(cmd.isCommandHubInfo());
		EXO_ASSERT(!cmd.getSourceID());
		EXO_ASSERT(args(cmd) == "ADBASE|ADTIGR");
	
	return 1;
});

/* And one with no arguments whatsoever. */
EXO_TEST(adcparser_a_command_with_no_arguments_parses,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "IINF"));
		EXO_ASSERT(cmd.countArguments() == 0);
	
	return 1;
});

/* A broadcast names its source, which is taken off the front. */
EXO_TEST(adcparser_a_broadcast_takes_its_source_off_the_front,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "BINF AAAA NIbob SL3"));
		EXO_ASSERT(cmd.isCommandBroadcast());
		EXO_ASSERT(cmd.getSourceID() && std::string(cmd.getSourceID()) == "AAAA");
		EXO_ASSERT(!cmd.getTargetID());
		EXO_ASSERT(args(cmd) == "NIbob|SL3");
	
	return 1;
});

/* A direct message names both ends. */
EXO_TEST(adcparser_a_direct_message_takes_both_of_its_ids,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "DMSG AAAA BBBB hello"));
		EXO_ASSERT(cmd.isCommandDirect());
		EXO_ASSERT(std::string(cmd.getSourceID()) == "AAAA");
		EXO_ASSERT(std::string(cmd.getTargetID()) == "BBBB");
		EXO_ASSERT(args(cmd) == "hello");
	
	return 1;
});

/* An echo is addressed the same way. */
EXO_TEST(adcparser_an_echo_takes_both_of_its_ids,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "EMSG AAAA BBBB hi"));
		EXO_ASSERT(cmd.isCommandEcho());
		EXO_ASSERT(std::string(cmd.getSourceID()) == "AAAA");
		EXO_ASSERT(std::string(cmd.getTargetID()) == "BBBB");
	
	return 1;
});

/* A prefix that needs IDs and has not got them is not a command. */
EXO_TEST(adcparser_a_command_missing_the_ids_its_prefix_needs_is_refused,
{
		ADC::Command a, b, c;
		EXO_ASSERT(!PARSE(a, "BINF"));
		EXO_ASSERT(!PARSE(b, "DMSG AAAA"));
		EXO_ASSERT(!PARSE(c, "EMSG AAAA"));
	
	return 1;
});

/* A prefix nobody speaks. */
EXO_TEST(adcparser_an_unknown_prefix_is_refused,
{
		ADC::Command a, b;
		EXO_ASSERT(!PARSE(a, "ZZZZ AAAA"));
		EXO_ASSERT(!PARSE(b, "1234 AAAA"));
	
	return 1;
});

/*
	 * A NUL inside the line makes buflen and the terminator disagree, and every
	 * offset here is taken from buflen.
	 */
EXO_TEST(adcparser_a_line_containing_a_nul_is_refused,
{
		ADC::Command cmd;
		EXO_ASSERT(!PARSE(cmd, "BINF AAAA\0NIbob"));
	
	return 1;
});

/*
	 * buflen is the authority, not a terminator: the caller's buffer holds the
	 * next line as well, and none of it may leak into this command.
	 */
EXO_TEST(adcparser_nothing_past_buflen_reaches_the_command,
{
		const char raw[] = "BINF AAAA NIbob\nBMSG AAAA hello";
		ADC::Command cmd;
		EXO_ASSERT(ADC::Parser::parse(raw, 15, cmd));
		EXO_ASSERT(args(cmd) == "NIbob");
		EXO_ASSERT(std::string(cmd.getRawCommand()) == "BINF AAAA NIbob\n");
	
	return 1;
});

/* The fourcc is built from unsigned bytes. */
EXO_TEST(adcparser_a_byte_with_the_top_bit_set_does_not_sign_extend,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "B\xc3\x4e\x46 AAAA"));
		EXO_ASSERT(cmd.getFourcc() == 0x42c34e46u);
	
	return 1;
});

/* The escapes ADC defines. */
EXO_TEST(adcparser_the_escapes_are_decoded,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "IMSG hello\\sworld\\nagain\\\\done"));
		EXO_ASSERT(args(cmd) == "hello world\nagain\\done");
	
	return 1;
});

/* An escape ADC does not define keeps the character. */
EXO_TEST(adcparser_an_unknown_escape_keeps_its_character,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "IMSG a\\qb"));
		EXO_ASSERT(args(cmd) == "aqb");
	
	return 1;
});

/* An escaped space does not split an argument. */
EXO_TEST(adcparser_an_escaped_space_does_not_split_an_argument,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "BMSG AAAA how\\sare\\syou"));
		EXO_ASSERT(cmd.countArguments() == 1);
		EXO_ASSERT(args(cmd) == "how are you");
	
	return 1;
});

/* Named arguments are found by their two letter name. */
EXO_TEST(adcparser_named_arguments_are_found_by_name,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "BINF AAAA NIbob HN2 SL3"));

		EXO_ASSERT(cmd.haveArgument("NI", 2));
		EXO_ASSERT(cmd.haveArgument("SL", 2));
		EXO_ASSERT(!cmd.haveArgument("XX", 2));

		const char* nick = cmd.getArgument("NI", 2);
		EXO_ASSERT(nick && std::string(nick) == "bob");
		EXO_ASSERT(std::string(cmd.getArgument("HN", 2)) == "2");
		EXO_ASSERT(!cmd.getArgument("XX", 2));
	
	return 1;
});

/* An argument that is only a name has an empty value, not a missing one. */
EXO_TEST(adcparser_an_argument_with_no_value_is_still_present,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "BINF AAAA NI"));
		EXO_ASSERT(cmd.haveArgument("NI", 2));
		const char* nick = cmd.getArgument("NI", 2);
		EXO_ASSERT(nick && !*nick);
	
	return 1;
});

/* Searching from an offset skips what came before it. */
EXO_TEST(adcparser_a_search_from_an_offset_skips_the_earlier_ones,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "BINF AAAA NIbob NIbill"));
		EXO_ASSERT(std::string(cmd.getArgument("NI", 2, 0)) == "bob");
		EXO_ASSERT(std::string(cmd.getArgument("NI", 2, 1)) == "bill");
		EXO_ASSERT(!cmd.getArgument("NI", 2, 2));
	
	return 1;
});

/* Removing takes every match with it and leaves the order alone. */
EXO_TEST(adcparser_removing_an_argument_takes_every_match,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "BINF AAAA NIbob HN2 NIbill SL3"));

		EXO_ASSERT(cmd.removeArgument("NI", 2));
		EXO_ASSERT(args(cmd) == "HN2|SL3");
		EXO_ASSERT(!cmd.removeArgument("NI", 2));
		EXO_ASSERT(!cmd.removeArgument("XX", 2));
	
	return 1;
});

/* A feature broadcast carries a list of features either way. */
EXO_TEST(adcparser_a_feature_broadcast_takes_its_feature_list_off_the_front,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "FSCH AAAA +TCP4-NAT0 ANmovie"));
		EXO_ASSERT(cmd.isCommandFeature());
		EXO_ASSERT(std::string(cmd.getSourceID()) == "AAAA");

		/* The feature list is not an argument; it is taken off with the IDs. */
		EXO_ASSERT(args(cmd) == "ANmovie");
	
	return 1;
});

/* A feature list that is not one. */
EXO_TEST(adcparser_a_malformed_feature_list_is_refused,
{

		const char* bad[] = {
			"FSCH AAAA TCP4 ANx",        /* no sign */
			"FSCH AAAA +TCP ANx",        /* too short */
			"FSCH AAAA +TCP44 ANx",      /* too long */
			"FSCH AAAA +tcp4 ANx",       /* not upper case */
			"FSCH AAAA +TCP4-NAT ANx",   /* the second one is short */
			"FSCH AAAA * ANx",
			0
		};

		for (size_t n = 0; bad[n]; n++)
		{
			ADC::Command cmd;
			if (ADC::Parser::parse(bad[n], strlen(bad[n]), cmd))
			{
				const std::string why = std::string("accepted \"") + bad[n] + "\"";
				EXO_FAIL(why.c_str());
			}
		}
	
	return 1;
});

/* A feature broadcast with no feature list at all. */
EXO_TEST(adcparser_a_feature_broadcast_with_no_features_is_refused,
{
		ADC::Command cmd;
		EXO_ASSERT(!PARSE(cmd, "FSCH AAAA"));
	
	return 1;
});

/*
	 * The fifth byte is the separator. Without checking it, "BINFX AAAA" is
	 * read as a BINF whose source ID is the empty string - which is to say a
	 * broadcast from nobody, rather than the malformed line it is.
	 */
EXO_TEST(adcparser_a_command_name_not_followed_by_a_space_is_refused,
{
		ADC::Command a, b;
		EXO_ASSERT(!PARSE(a, "BINFX AAAA"));
		EXO_ASSERT(!PARSE(b, "IINFX"));
	
	return 1;
});

/* What a parsed command puts back on the wire is what came in. */
EXO_TEST(adcparser_a_parsed_command_writes_back_the_line_it_came_from,
{

		const char* lines[] = {
			"ISUP ADBASE ADTIGR",
			"BINF AAAA NIbob HN2",
			"DMSG AAAA BBBB hello\\sthere",
			"EMSG AAAA BBBB hi",
			"HSUP ADBASE",
			"CGET file files.xml.bz2 0 -1",
			0
		};

		for (size_t n = 0; lines[n]; n++)
		{
			ADC::Command cmd;
			if (!ADC::Parser::parse(lines[n], strlen(lines[n]), cmd))
			{
				const std::string why = std::string("refused \"") + lines[n] + "\"";
				EXO_FAIL(why.c_str());
			}

			const std::string expect = std::string(lines[n]) + "\n";
			if (written(cmd) != expect)
			{
				const std::string why = std::string("\"") + lines[n]
					+ "\" came back as \"" + shown(written(cmd)) + "\"";
				EXO_FAIL(why.c_str());
			}
		}
	
	return 1;
});

/*
	 * And still does once the cache is dropped, which is when the arguments
	 * rather than the remembered line are what gets serialised.
	 */
EXO_TEST(adcparser_a_parsed_command_writes_itself_from_its_parts_too,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "BINF AAAA NIbob HN2"));
		cmd.resetCache();
		EXO_ASSERT(written(cmd) == "BINF AAAA NIbob HN2\n");
	
	return 1;
});

/* A value carrying the characters that need escaping survives both ways. */
EXO_TEST(adcparser_a_value_needing_escapes_survives_the_round_trip,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "BMSG AAAA a\\sb\\nc\\\\d"));
		EXO_ASSERT(args(cmd) == "a b\nc\\d");

		cmd.resetCache();
		EXO_ASSERT(written(cmd) == "BMSG AAAA a\\sb\\nc\\\\d\n");

		/* And parsing what we wrote gives the same value back. */
		const std::string wire = written(cmd);
		ADC::Command again;
		EXO_ASSERT(ADC::Parser::parse(wire.c_str(), wire.size() - 1, again));
		EXO_ASSERT(args(again) == "a b\nc\\d");
	
	return 1;
});

/*
	 * A feature broadcast with nothing to exclude. write() wanted both lists to
	 * be non-empty, so an FSCH that only says which features a client must have
	 * - the ordinary case - serialised to nothing at all.
	 */
EXO_TEST(adcparser_a_feature_broadcast_with_only_inclusions_is_written,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "FSCH AAAA +TCP4 ANmovie"));
		cmd.resetCache();
		EXO_ASSERT(written(cmd) == "FSCH AAAA +TCP4 ANmovie\n");
	
	return 1;
});

/* And one with nothing to require. */
EXO_TEST(adcparser_a_feature_broadcast_with_only_exclusions_is_written,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "FSCH AAAA -NAT0 ANmovie"));
		cmd.resetCache();
		EXO_ASSERT(written(cmd) == "FSCH AAAA -NAT0 ANmovie\n");
	
	return 1;
});

/* Both lists, in the order they are written. */
EXO_TEST(adcparser_both_feature_lists_are_written_in_order,
{
		ADC::Command cmd;
		EXO_ASSERT(PARSE(cmd, "FSCH AAAA +TCP4+TIGR-NAT0 ANmovie"));
		cmd.resetCache();
		EXO_ASSERT(written(cmd) == "FSCH AAAA +TCP4+TIGR-NAT0 ANmovie\n");
	
	return 1;
});

/*
	 * A copy owns its own strings. The feature lists were copied as pointers
	 * while everything else was duplicated, so the two commands shared them -
	 * and both destructors free them.
	 */
EXO_TEST(adcparser_a_copied_command_owns_its_own_feature_lists,
{
		ADC::Command* original = new ADC::Command();
		EXO_ASSERT(PARSE(*original, "FSCH AAAA +TCP4-NAT0 ANmovie"));

		ADC::Command* copy = new ADC::Command(original);
		delete original;

		/* Reading the copy now is reading the original's strings, if shared. */
		copy->resetCache();
		EXO_ASSERT(written(*copy) == "FSCH AAAA +TCP4-NAT0 ANmovie\n");

		/* And this frees them a second time. */
		delete copy;
	
	return 1;
});
