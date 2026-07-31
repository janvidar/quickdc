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
#include "network/adc/parser.h"

#include <samurai/io/buffer.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int failures = 0;

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

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

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

int main()
{
	/* Commands whose prefix needs no ID. */
	{
		ADC::Command cmd(FOURCC('I','S','I','D'));
		check("no ids required", "ISID\n", cmd);
	}

	{
		ADC::Command cmd(FOURCC('H','S','U','P'));
		cmd.addArgument("AD", "BASE");
		cmd.addArgument("RM", "TIGR");
		check("with arguments", "HSUP ADBASE RMTIGR\n", cmd);
	}

	{
		ADC::Command cmd(FOURCC('B','I','N','F'), "AAAA");
		cmd.addArgument("NI", "nick");
		check("broadcast with a source", "BINF AAAA NInick\n", cmd);
	}

	{
		/* A rich text chat line: the body is escaped, the flag follows it. */
		ADC::Command cmd(FOURCC('B','M','S','G'), "AAAA");
		cmd.addArgument("**found it** in `~/Music`");
		cmd.addArgument("RT", "1");
		check("rich text broadcast",
			"BMSG AAAA **found\\sit**\\sin\\s`~/Music` RT1\n", cmd);
	}

	{
		ADC::Command cmd(FOURCC('D','M','S','G'), "AAAA", "BBBB");
		cmd.addArgument("hello");
		check("direct with both ids", "DMSG AAAA BBBB hello\n", cmd);
	}

	/* A space in an argument has to come back escaped. */
	{
		ADC::Command cmd(FOURCC('B','M','S','G'), "AAAA");
		cmd.addArgument("two words");
		check("argument escaping", "BMSG AAAA two\\swords\n", cmd);
	}

	/*
	 * Incomplete commands. write() gives up on these, and the point is that
	 * asking twice behaves the same as asking once and that nothing is left
	 * half written into a caller's buffer.
	 */
	{
		ADC::Command cmd(FOURCC('B','I','N','F'));
		CHECK(cmd.getSize() == 0);
		CHECK(cmd.getRawCommand() != 0);
		CHECK(std::string(cmd.getRawCommand()).empty());
		CHECK(written(cmd).empty());

		/* Repeated, because each call re-runs write() while the cache is empty. */
		for (int n = 0; n < 3; n++)
		{
			CHECK(cmd.getSize() == 0);
			CHECK(written(cmd).empty());
		}
		printf("  ok %-34s\n", "broadcast with no source");
	}

	{
		ADC::Command cmd(FOURCC('D','M','S','G'), "AAAA");
		CHECK(cmd.getSize() == 0);
		CHECK(written(cmd).empty());
		printf("  ok %-34s\n", "direct with no target");
	}

	/* A caller's own buffer must not be disturbed by a command it cannot write. */
	{
		ADC::Command cmd(FOURCC('B','I','N','F'));
		Samurai::IO::Buffer buf;
		buf.append("keep", 4);
		cmd.write(&buf);
		CHECK(buf.copyRange(0, buf.size()) == "keep");
		printf("  ok %-34s\n", "incomplete leaves the buffer");
	}

	/* Appending to a buffer that already holds something starts where it ended. */
	{
		ADC::Command cmd(FOURCC('I','S','I','D'));
		Samurai::IO::Buffer buf;
		buf.append("head ", 5);
		cmd.write(&buf);
		CHECK(buf.copyRange(0, buf.size()) == "head ISID\n");
		CHECK(std::string(cmd.getRawCommand()) == "ISID\n");
		printf("  ok %-34s\n", "appends after existing data");
	}

	/* Changing the command drops the cache rather than serving a stale answer. */
	{
		ADC::Command cmd(FOURCC('B','M','S','G'), "AAAA");
		cmd.addArgument("one");
		CHECK(std::string(cmd.getRawCommand()) == "BMSG AAAA one\n");

		cmd.addArgument("two");
		CHECK(std::string(cmd.getRawCommand()) == "BMSG AAAA one two\n");
		CHECK(cmd.getSize() == strlen("BMSG AAAA one two\n"));

		CHECK(cmd.removeArgument("on", 2));
		CHECK(std::string(cmd.getRawCommand()) == "BMSG AAAA two\n");
		printf("  ok %-34s\n", "cache follows the arguments");
	}

	/*
	 * Asking an abandoned command for its bytes must not cost anything. Each
	 * call re-runs write(), because there is no cache for it to stop at.
	 */
	{
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
			printf("  ok %-34s\n", "abandoned command allocates nothing");
		}
	}

	/* The same for one that does serialise, once its cache is warm. */
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
		CHECK(live == before);
		printf("  ok %-34s\n", "cached command allocates nothing");
	}

	/* A parsed command caches the line it came from. */
	{
		ADC::Command cmd;
		const char* line = "BMSG AAAA hello";
		CHECK(ADC::Parser::parse(line, strlen(line), cmd));
		CHECK(std::string(cmd.getRawCommand()) == "BMSG AAAA hello\n");
		CHECK(cmd.getSize() == strlen("BMSG AAAA hello\n"));
		printf("  ok %-34s\n", "parsed command keeps its line");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
