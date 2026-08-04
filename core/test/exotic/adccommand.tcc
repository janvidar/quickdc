/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Covers ADC::Command's serialisation: what write() puts on the wire, that the
 * cache and the size agree with it, and that the cache is dropped when the
 * command changes.
 *
 * The awkward case is a command that cannot be serialised at all - a broadcast
 * with no source ID, say. write() abandons it, so it has to leave a caller that
 * asked for its bytes with something coherent rather than a half command.
 */

#include "quickdc.h"
#include "network/adc/adccommands.h"
#include "network/adc/parser.h"

#include <samurai/io/buffer.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

/*
 * Counting every allocation in the binary is what makes the abandoned-command
 * path testable: write() builds its own buffer when the caller supplies none,
 * and a return that skipped freeing it left nothing else to observe.
 */
static size_t live = 0;

void* operator new(size_t n)
{
	void* p = malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	live++;
	return p;
}

void operator delete(void* p) noexcept
{
	if (!p) return;
	live--;
	free(p);
}

void* operator new[](size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

static std::string written(ADC::Command& cmd)
{
	Samurai::IO::Buffer buf;
	cmd.write(&buf);
	return buf.copyRange(0, buf.size());
}

static void check(const char* label, const char* expect, ADC::Command& cmd)
{
	const std::string wire = written(cmd);
	const char* raw = cmd.getRawCommand();
	const size_t size = cmd.getSize();

	bool ok = true;

	if (wire != expect)
	{
		printf("FAIL %s: write() gave '%s', expected '%s'\n", label, wire.c_str(), expect);
		ok = false;
	}

	/* getRawCommand() and getSize() are the same bytes by another route. */
	if (!raw || std::string(raw) != expect)
	{
		printf("FAIL %s: getRawCommand() gave '%s', expected '%s'\n",
			label, raw ? raw : "(null)", expect);
		ok = false;
	}

	if (size != strlen(expect))
	{
		printf("FAIL %s: getSize() gave %zu, expected %zu\n", label, size, strlen(expect));
		ok = false;
	}

	if (!ok)
	{
		failures++;
		return;
	}

	std::string shown;
	for (const char* p = expect; *p; p++)
		if (*p == '\n') shown += "\\n"; else shown += *p;
	printf("  ok %-34s '%s'\n", label, shown.c_str());
}

EXO_TEST(adccommand_,
{
		/* Our own hub answers out of the same branch, so its ISUP carries it too. */
		ADC::Command cmd(FOURCC('I','S','U','P'));
		ADC::addSupportFlags(cmd, false);
		EXO_ASSERT(written(cmd).find("AD" ADC_EXT_RICHTEXT) != std::string::npos);
	
	return 1;
});

/*
	 * Incomplete commands. write() gives up on these, and the point is that
	 * asking twice behaves the same as asking once and that nothing is left
	 * half written into a caller's buffer.
	 */
EXO_TEST(adccommand__2,
{
		ADC::Command cmd(FOURCC('B','I','N','F'));
		EXO_ASSERT(cmd.getSize() == 0);
		EXO_ASSERT(cmd.getRawCommand() != 0);
		EXO_ASSERT(std::string(cmd.getRawCommand()).empty());
		EXO_ASSERT(written(cmd).empty());

		/* Repeated, because each call re-runs write() while the cache is empty. */
		for (int n = 0; n < 3; n++)
		{
			EXO_ASSERT(cmd.getSize() == 0);
			EXO_ASSERT(written(cmd).empty());
		}
	
	return 1;
});

EXO_TEST(adccommand__2_2,
{
		ADC::Command cmd(FOURCC('D','M','S','G'), "AAAA");
		EXO_ASSERT(cmd.getSize() == 0);
		EXO_ASSERT(written(cmd).empty());
	
	return 1;
});

/* A caller's own buffer must not be disturbed by a command it cannot write. */
EXO_TEST(adccommand__2_2_2,
{
		ADC::Command cmd(FOURCC('B','I','N','F'));
		Samurai::IO::Buffer buf;
		buf.append("keep", 4);
		cmd.write(&buf);
		EXO_ASSERT(buf.copyRange(0, buf.size()) == "keep");
	
	return 1;
});

/* Appending to a buffer that already holds something starts where it ended. */
EXO_TEST(adccommand__2_2_2_2,
{
		ADC::Command cmd(FOURCC('I','S','I','D'));
		Samurai::IO::Buffer buf;
		buf.append("head ", 5);
		cmd.write(&buf);
		EXO_ASSERT(buf.copyRange(0, buf.size()) == "head ISID\n");
		EXO_ASSERT(std::string(cmd.getRawCommand()) == "ISID\n");
	
	return 1;
});

/* Changing the command drops the cache rather than serving a stale answer. */
EXO_TEST(adccommand__2_2_2_2_2,
{
		ADC::Command cmd(FOURCC('B','M','S','G'), "AAAA");
		cmd.addArgument("one");
		EXO_ASSERT(std::string(cmd.getRawCommand()) == "BMSG AAAA one\n");

		cmd.addArgument("two");
		EXO_ASSERT(std::string(cmd.getRawCommand()) == "BMSG AAAA one two\n");
		EXO_ASSERT(cmd.getSize() == strlen("BMSG AAAA one two\n"));

		EXO_ASSERT(cmd.removeArgument("on", 2));
		EXO_ASSERT(std::string(cmd.getRawCommand()) == "BMSG AAAA two\n");
	
	return 1;
});

/*
	 * Asking an abandoned command for its bytes must not cost anything. Each
	 * call re-runs write(), because there is no cache for it to stop at.
	 */
EXO_TEST(adccommand__2_2_2_2_2_2,
{
	/* TCCIFY: reports failure or a skip its own way. Convert by hand
	   to EXO_FAIL(msg) / EXO_SKIP(reason) - this test cannot fail
	   as it stands. */

		ADC::Command cmd(FOURCC('B','I','N','F'));
		cmd.getSize();

		const size_t before = live;
		for (int n = 0; n < 100; n++)
		{
			cmd.getSize();
			cmd.getRawCommand();
		}
		const size_t after = live;

		if (after != before)
		{
			printf("FAIL abandoned command leaks: %zu allocations over 200 calls\n",
				after - before);
			failures++;
		}
		else
		{
		}
	
	return 1;
});

/* The same for one that does serialise, once its cache is warm. */
EXO_TEST(adccommand__2_2_2_2_2_2_2,
{
		ADC::Command cmd(FOURCC('B','M','S','G'), "AAAA");
		cmd.addArgument("hello");
		cmd.getSize();

		const size_t before = live;
		for (int n = 0; n < 100; n++)
		{
			cmd.getSize();
			cmd.getRawCommand();
		}
		EXO_ASSERT(live == before);
	
	return 1;
});

/* A parsed command caches the line it came from. */
EXO_TEST(adccommand__2_2_2_2_2_2_2_2,
{
		ADC::Command cmd;
		const char* line = "BMSG AAAA hello";
		EXO_ASSERT(ADC::Parser::parse(line, strlen(line), cmd));
		EXO_ASSERT(std::string(cmd.getRawCommand()) == "BMSG AAAA hello\n");
		EXO_ASSERT(cmd.getSize() == strlen("BMSG AAAA hello\n"));
	
	return 1;
});
