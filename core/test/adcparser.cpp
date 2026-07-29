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

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

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

int main()
{
	setbuf(stdout, 0);

	/* Too short to hold a command name. */
	{
		ADC::Command cmd;
		CHECK(!ADC::Parser::parse(0, 10, cmd));
		CHECK(!PARSE(cmd, ""));
		CHECK(!PARSE(cmd, "I"));
		CHECK(!PARSE(cmd, "IIN"));
		printf("  ok a line too short to name a command is refused\n");
	}

	/* A command whose prefix needs no ID at all. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "ISUP ADBASE ADTIGR"));
		CHECK(cmd.getFourcc() == FOURCC('I','S','U','P'));
		CHECK(cmd.isCommandHubInfo());
		CHECK(!cmd.getSourceID());
		CHECK(args(cmd) == "ADBASE|ADTIGR");
		printf("  ok an info command keeps all of its arguments\n");
	}

	/* And one with no arguments whatsoever. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "IINF"));
		CHECK(cmd.countArguments() == 0);
		printf("  ok a command with no arguments parses\n");
	}

	/* A broadcast names its source, which is taken off the front. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "BINF AAAA NIbob SL3"));
		CHECK(cmd.isCommandBroadcast());
		CHECK(cmd.getSourceID() && std::string(cmd.getSourceID()) == "AAAA");
		CHECK(!cmd.getTargetID());
		CHECK(args(cmd) == "NIbob|SL3");
		printf("  ok a broadcast takes its source off the front\n");
	}

	/* A direct message names both ends. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "DMSG AAAA BBBB hello"));
		CHECK(cmd.isCommandDirect());
		CHECK(std::string(cmd.getSourceID()) == "AAAA");
		CHECK(std::string(cmd.getTargetID()) == "BBBB");
		CHECK(args(cmd) == "hello");
		printf("  ok a direct message takes both of its IDs\n");
	}

	/* An echo is addressed the same way. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "EMSG AAAA BBBB hi"));
		CHECK(cmd.isCommandEcho());
		CHECK(std::string(cmd.getSourceID()) == "AAAA");
		CHECK(std::string(cmd.getTargetID()) == "BBBB");
		printf("  ok an echo takes both of its IDs\n");
	}

	/* A prefix that needs IDs and has not got them is not a command. */
	{
		ADC::Command a, b, c;
		CHECK(!PARSE(a, "BINF"));
		CHECK(!PARSE(b, "DMSG AAAA"));
		CHECK(!PARSE(c, "EMSG AAAA"));
		printf("  ok a command missing the IDs its prefix needs is refused\n");
	}

	/* A prefix nobody speaks. */
	{
		ADC::Command a, b;
		CHECK(!PARSE(a, "ZZZZ AAAA"));
		CHECK(!PARSE(b, "1234 AAAA"));
		printf("  ok an unknown prefix is refused\n");
	}

	/*
	 * A NUL inside the line makes buflen and the terminator disagree, and every
	 * offset here is taken from buflen.
	 */
	{
		ADC::Command cmd;
		CHECK(!PARSE(cmd, "BINF AAAA\0NIbob"));
		printf("  ok a line containing a NUL is refused\n");
	}

	/*
	 * buflen is the authority, not a terminator: the caller's buffer holds the
	 * next line as well, and none of it may leak into this command.
	 */
	{
		const char raw[] = "BINF AAAA NIbob\nBMSG AAAA hello";
		ADC::Command cmd;
		CHECK(ADC::Parser::parse(raw, 15, cmd));
		CHECK(args(cmd) == "NIbob");
		CHECK(std::string(cmd.getRawCommand()) == "BINF AAAA NIbob\n");
		printf("  ok nothing past buflen reaches the command\n");
	}

	/* The fourcc is built from unsigned bytes. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "B\xc3\x4e\x46 AAAA"));
		CHECK(cmd.getFourcc() == 0x42c34e46u);
		printf("  ok a byte with the top bit set does not sign extend\n");
	}

	/* The escapes ADC defines. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "IMSG hello\\sworld\\nagain\\\\done"));
		CHECK(args(cmd) == "hello world\nagain\\done");
		printf("  ok the escapes are decoded\n");
	}

	/* An escape ADC does not define keeps the character. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "IMSG a\\qb"));
		CHECK(args(cmd) == "aqb");
		printf("  ok an unknown escape keeps its character\n");
	}

	/* An escaped space does not split an argument. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "BMSG AAAA how\\sare\\syou"));
		CHECK(cmd.countArguments() == 1);
		CHECK(args(cmd) == "how are you");
		printf("  ok an escaped space does not split an argument\n");
	}

	/* Named arguments are found by their two letter name. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "BINF AAAA NIbob HN2 SL3"));

		CHECK(cmd.haveArgument("NI", 2));
		CHECK(cmd.haveArgument("SL", 2));
		CHECK(!cmd.haveArgument("XX", 2));

		const char* nick = cmd.getArgument("NI", 2);
		CHECK(nick && std::string(nick) == "bob");
		CHECK(std::string(cmd.getArgument("HN", 2)) == "2");
		CHECK(!cmd.getArgument("XX", 2));
		printf("  ok named arguments are found by name\n");
	}

	/* An argument that is only a name has an empty value, not a missing one. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "BINF AAAA NI"));
		CHECK(cmd.haveArgument("NI", 2));
		const char* nick = cmd.getArgument("NI", 2);
		CHECK(nick && !*nick);
		printf("  ok an argument with no value is still present\n");
	}

	/* Searching from an offset skips what came before it. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "BINF AAAA NIbob NIbill"));
		CHECK(std::string(cmd.getArgument("NI", 2, 0)) == "bob");
		CHECK(std::string(cmd.getArgument("NI", 2, 1)) == "bill");
		CHECK(!cmd.getArgument("NI", 2, 2));
		printf("  ok a search from an offset skips the earlier ones\n");
	}

	/* Removing takes every match with it and leaves the order alone. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "BINF AAAA NIbob HN2 NIbill SL3"));

		CHECK(cmd.removeArgument("NI", 2));
		CHECK(args(cmd) == "HN2|SL3");
		CHECK(!cmd.removeArgument("NI", 2));
		CHECK(!cmd.removeArgument("XX", 2));
		printf("  ok removing an argument takes every match\n");
	}

	/* A feature broadcast carries a list of features either way. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "FSCH AAAA +TCP4-NAT0 ANmovie"));
		CHECK(cmd.isCommandFeature());
		CHECK(std::string(cmd.getSourceID()) == "AAAA");

		/* The feature list is not an argument; it is taken off with the IDs. */
		CHECK(args(cmd) == "ANmovie");
		printf("  ok a feature broadcast takes its feature list off the front\n");
	}

	/* A feature list that is not one. */
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
				printf("FAIL %s:%d accepted \"%s\"\n", __FILE__, __LINE__, bad[n]);
				failures++;
			}
		}
		printf("  ok a malformed feature list is refused\n");
	}

	/* A feature broadcast with no feature list at all. */
	{
		ADC::Command cmd;
		CHECK(!PARSE(cmd, "FSCH AAAA"));
		printf("  ok a feature broadcast with no features is refused\n");
	}

	/*
	 * The fifth byte is the separator. Without checking it, "BINFX AAAA" is
	 * read as a BINF whose source ID is the empty string - which is to say a
	 * broadcast from nobody, rather than the malformed line it is.
	 */
	{
		ADC::Command a, b;
		CHECK(!PARSE(a, "BINFX AAAA"));
		CHECK(!PARSE(b, "IINFX"));
		printf("  ok a command name not followed by a space is refused\n");
	}

	/* What a parsed command puts back on the wire is what came in. */
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
				printf("FAIL %s:%d refused \"%s\"\n", __FILE__, __LINE__, lines[n]);
				failures++;
				continue;
			}

			const std::string expect = std::string(lines[n]) + "\n";
			if (written(cmd) != expect)
			{
				printf("FAIL %s:%d \"%s\" came back as \"%s\"\n", __FILE__, __LINE__,
					lines[n], shown(written(cmd)).c_str());
				failures++;
			}
		}
		printf("  ok a parsed command writes back the line it came from\n");
	}

	/*
	 * And still does once the cache is dropped, which is when the arguments
	 * rather than the remembered line are what gets serialised.
	 */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "BINF AAAA NIbob HN2"));
		cmd.resetCache();
		CHECK(written(cmd) == "BINF AAAA NIbob HN2\n");
		printf("  ok a parsed command writes itself from its parts too\n");
	}

	/* A value carrying the characters that need escaping survives both ways. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "BMSG AAAA a\\sb\\nc\\\\d"));
		CHECK(args(cmd) == "a b\nc\\d");

		cmd.resetCache();
		CHECK(written(cmd) == "BMSG AAAA a\\sb\\nc\\\\d\n");

		/* And parsing what we wrote gives the same value back. */
		const std::string wire = written(cmd);
		ADC::Command again;
		CHECK(ADC::Parser::parse(wire.c_str(), wire.size() - 1, again));
		CHECK(args(again) == "a b\nc\\d");
		printf("  ok a value needing escapes survives the round trip\n");
	}

	/*
	 * A feature broadcast with nothing to exclude. write() wanted both lists to
	 * be non-empty, so an FSCH that only says which features a client must have
	 * - the ordinary case - serialised to nothing at all.
	 */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "FSCH AAAA +TCP4 ANmovie"));
		cmd.resetCache();
		CHECK(written(cmd) == "FSCH AAAA +TCP4 ANmovie\n");
		printf("  ok a feature broadcast with only inclusions is written\n");
	}

	/* And one with nothing to require. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "FSCH AAAA -NAT0 ANmovie"));
		cmd.resetCache();
		CHECK(written(cmd) == "FSCH AAAA -NAT0 ANmovie\n");
		printf("  ok a feature broadcast with only exclusions is written\n");
	}

	/* Both lists, in the order they are written. */
	{
		ADC::Command cmd;
		CHECK(PARSE(cmd, "FSCH AAAA +TCP4+TIGR-NAT0 ANmovie"));
		cmd.resetCache();
		CHECK(written(cmd) == "FSCH AAAA +TCP4+TIGR-NAT0 ANmovie\n");
		printf("  ok both feature lists are written in order\n");
	}

	/*
	 * A copy owns its own strings. The feature lists were copied as pointers
	 * while everything else was duplicated, so the two commands shared them -
	 * and both destructors free them.
	 */
	{
		ADC::Command* original = new ADC::Command();
		CHECK(PARSE(*original, "FSCH AAAA +TCP4-NAT0 ANmovie"));

		ADC::Command* copy = new ADC::Command(original);
		delete original;

		/* Reading the copy now is reading the original's strings, if shared. */
		copy->resetCache();
		CHECK(written(*copy) == "FSCH AAAA +TCP4-NAT0 ANmovie\n");

		/* And this frees them a second time. */
		delete copy;
		printf("  ok a copied command owns its own feature lists\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
