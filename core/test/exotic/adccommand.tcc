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

#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>

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

static bool check(const char* label, const char* expect, ADC::Command& cmd)
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

	return ok;
}

/* Commands whose prefix needs no ID. */
EXO_TEST(adccommand_no_ids_required,
{
		ADC::Command cmd(FOURCC('I','S','I','D'));
		EXO_ASSERT(check("no ids required", "ISID\n", cmd));
	
	return 1;
});

EXO_TEST(adccommand_with_arguments,
{
		ADC::Command cmd(FOURCC('H','S','U','P'));
		cmd.addArgument("AD", "BASE");
		cmd.addArgument("RM", "TIGR");
		EXO_ASSERT(check("with arguments", "HSUP ADBASE RMTIGR\n", cmd));
	
	return 1;
});

EXO_TEST(adccommand_broadcast_with_a_source,
{
		ADC::Command cmd(FOURCC('B','I','N','F'), "AAAA");
		cmd.addArgument("NI", "nick");
		EXO_ASSERT(check("broadcast with a source", "BINF AAAA NInick\n", cmd));
	
	return 1;
});

EXO_TEST(adccommand_rich_text_broadcast,
{
		/* A rich text chat line: the body is escaped, the flag follows it. */
		ADC::Command cmd(FOURCC('B','M','S','G'), "AAAA");
		cmd.addArgument("**found it** in `~/Music`");
		cmd.addArgument("RT", "1");
		EXO_ASSERT(check("rich text broadcast",
			"BMSG AAAA **found\\sit**\\sin\\s`~/Music` RT1\n", cmd));
	
	return 1;
});

/*
	 * The support flags. Rich text is a hub extension, so it belongs in the SUP a
	 * client sends to a hub and in the one a hub answers with, and nowhere in the
	 * one a client sends to another client - the RT flag only travels via a hub.
	 *
	 * PING is in none of them. It says the sender answers a pinger, which only a hub
	 * does, so our own hub adds it to its ISUP itself - as it does NATT, and for the
	 * same reason.
	 */
EXO_TEST(adccommand_support_flags_to_a_hub,
{
		ADC::Command cmd(FOURCC('H','S','U','P'));
		ADC::addSupportFlags(cmd, false);
		EXO_ASSERT(check("support flags to a hub",
			"HSUP ADBASE ADBAS0 ADTIGR ADPFSR ADADCS AD"
			ADC_EXT_RICHTEXT "\n", cmd));
	
	return 1;
});

/* Connecting to a hub does not claim to be one. */
EXO_TEST(adccommand_a_client_does_not_offer_to_answer_pingers,
{
		ADC::Command hub(FOURCC('H','S','U','P'));
		ADC::addSupportFlags(hub, false);
		EXO_ASSERT(written(hub).find("AD" ADC_EXT_PING) == std::string::npos);

		ADC::Command peer(FOURCC('C','S','U','P'));
		ADC::addSupportFlags(peer, true);
		EXO_ASSERT(written(peer).find("AD" ADC_EXT_PING) == std::string::npos);
	
	return 1;
});

EXO_TEST(adccommand_support_flags_to_a_client,
{
		ADC::Command cmd(FOURCC('C','S','U','P'));
		ADC::addSupportFlags(cmd, true);
		EXO_ASSERT(check("support flags to a client",
			"CSUP ADBASE ADBAS0 ADTIGR ADPFSR ADADCS ADBZIP ADZLIG\n", cmd));
	
	return 1;
});

EXO_TEST(adccommand_support_flags_from_our_hub,
{
		/* Our own hub answers out of the same branch, so its ISUP carries it too. */
		ADC::Command cmd(FOURCC('I','S','U','P'));
		ADC::addSupportFlags(cmd, false);
		EXO_ASSERT(written(cmd).find("AD" ADC_EXT_RICHTEXT) != std::string::npos);
	
	return 1;
});

EXO_TEST(adccommand_direct_with_both_ids,
{
		ADC::Command cmd(FOURCC('D','M','S','G'), "AAAA", "BBBB");
		cmd.addArgument("hello");
		EXO_ASSERT(check("direct with both ids", "DMSG AAAA BBBB hello\n", cmd));
	
	return 1;
});

/* A space in an argument has to come back escaped. */
EXO_TEST(adccommand_argument_escaping,
{
		ADC::Command cmd(FOURCC('B','M','S','G'), "AAAA");
		cmd.addArgument("two words");
		EXO_ASSERT(check("argument escaping", "BMSG AAAA two\\swords\n", cmd));
	
	return 1;
});

/*
	 * Incomplete commands. write() gives up on these, and the point is that
	 * asking twice behaves the same as asking once and that nothing is left
	 * half written into a caller's buffer.
	 */
EXO_TEST(adccommand_broadcast_with_no_source,
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

EXO_TEST(adccommand_direct_with_no_target,
{
		ADC::Command cmd(FOURCC('D','M','S','G'), "AAAA");
		EXO_ASSERT(cmd.getSize() == 0);
		EXO_ASSERT(written(cmd).empty());
	
	return 1;
});

/* A caller's own buffer must not be disturbed by a command it cannot write. */
EXO_TEST(adccommand_incomplete_leaves_the_buffer,
{
		ADC::Command cmd(FOURCC('B','I','N','F'));
		Samurai::IO::Buffer buf;
		buf.append("keep", 4);
		cmd.write(&buf);
		EXO_ASSERT(buf.copyRange(0, buf.size()) == "keep");
	
	return 1;
});

/* Appending to a buffer that already holds something starts where it ended. */
EXO_TEST(adccommand_appends_after_existing_data,
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
EXO_TEST(adccommand_cache_follows_the_arguments,
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
EXO_TEST(adccommand_abandoned_command_allocates_nothing,
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
			const std::string why = "abandoned command leaks: "
				+ std::to_string(after - before) + " allocations over 200 calls";
			EXO_FAIL(why.c_str());
		}
	
	return 1;
});

/* The same for one that does serialise, once its cache is warm. */
EXO_TEST(adccommand_cached_command_allocates_nothing,
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
EXO_TEST(adccommand_parsed_command_keeps_its_line,
{
		ADC::Command cmd;
		const char* line = "BMSG AAAA hello";
		EXO_ASSERT(ADC::Parser::parse(line, strlen(line), cmd));
		EXO_ASSERT(std::string(cmd.getRawCommand()) == "BMSG AAAA hello\n");
		EXO_ASSERT(cmd.getSize() == strlen("BMSG AAAA hello\n"));
	
	return 1;
});

static bool empty_command_from_poisoned_storage(unsigned char poison)
{
	alignas(ADC::Command) unsigned char storage[sizeof(ADC::Command)];
	memset(storage, poison, sizeof(storage));

	ADC::Command* cmd = new (static_cast<void*>(storage)) ADC::Command();
	const std::string wire = written(*cmd);
	cmd->~Command();

	if (wire == "\n") return true;

	printf("FAIL empty command over 0x%02x: gave '%s' (%zu bytes), expected a newline\n",
		poison, wire.c_str(), wire.size());
	return false;
}

EXO_TEST(adccommand_empty_command_is_a_bare_newline,
{
		ADC::Command cmd;
		EXO_ASSERT(check("empty command", "\n", cmd));

	return 1;
});

EXO_TEST(adccommand_empty_command_ignores_what_its_storage_held,
{
		EXO_ASSERT(empty_command_from_poisoned_storage('I'));
		EXO_ASSERT(empty_command_from_poisoned_storage('H'));
		EXO_ASSERT(empty_command_from_poisoned_storage('B'));
		EXO_ASSERT(empty_command_from_poisoned_storage('E'));
		EXO_ASSERT(empty_command_from_poisoned_storage(0xff));

	return 1;
});

static std::string capture_stderr_of(void (*body)())
{
	const std::string path = "/tmp/quickdc_adccommand_"
		+ std::to_string((int) getpid()) + ".stderr";

	fflush(stderr);
	const int saved = dup(2);
	const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (saved < 0 || fd < 0) return std::string();
	dup2(fd, 2);
	close(fd);

	body();

	fflush(stderr);
	dup2(saved, 2);
	close(saved);

	std::string out;
	FILE* fh = fopen(path.c_str(), "rb");
	if (fh)
	{
		char buf[4096];
		size_t got;
		while ((got = fread(buf, 1, sizeof(buf), fh)) > 0) out.append(buf, got);
		fclose(fh);
	}
	unlink(path.c_str());
	return out;
}

static size_t occurrences(const std::string& haystack, const char* needle)
{
	size_t seen = 0;
	for (size_t at = haystack.find(needle); at != std::string::npos;
	     at = haystack.find(needle, at + 1))
		seen++;
	return seen;
}

static void ask_an_abandoned_command_for_its_bytes()
{
	ADC::Command cmd(FOURCC('B','I','N','F'));
	for (int n = 0; n < 100; n++)
	{
		cmd.getSize();
		cmd.getRawCommand();
	}
}

EXO_TEST(adccommand_an_abandoned_command_is_reported,
{
		const std::string log = capture_stderr_of(ask_an_abandoned_command_for_its_bytes);

		EXO_ASSERT(log.find("BINF") != std::string::npos);
		EXO_ASSERT(log.find("it has no source ID") != std::string::npos);

	return 1;
});

EXO_TEST(adccommand_an_abandoned_command_is_reported_only_once,
{
		const std::string log = capture_stderr_of(ask_an_abandoned_command_for_its_bytes);
		const size_t seen = occurrences(log, "was not sent");

		if (seen != 1)
		{
			const std::string why = "200 calls on one abandoned command reported it "
				+ std::to_string(seen) + " times, expected once";
			EXO_FAIL(why.c_str());
		}

	return 1;
});
