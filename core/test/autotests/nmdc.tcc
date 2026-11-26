/*
 * The NMDC commands the download queue is driven through: the $ADCGET a peer
 * transfer starts with, and the $SR search reply a source is found from.
 */

#include <samurai/samurai.h>
#include <string.h>
#include <stdlib.h>

#include <samurai/io/file.h>
#include <string>
#include <samurai/posix.h>

#include "network/filesource.h"
#include "network/downloadqueue.h"
#include "network/dc/commands.h"
#include "network/dc/hubcommands.h"

#define QUEUE_TEST_TTH_A "MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
#define QUEUE_TEST_TTH_B "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

/*
 * The queue lives at "~/.quickdc/queue.xml", and File::resolvePath() expands
 * that '~' from the environment, so pointing HOME at a scratch directory is all
 * it takes to keep the one test below that needs a queue away from the real one.
 */

/* It carries the process id, because under DISCOVER each case is a process of
   its own, and the setup below removes the queue file it finds there. */
static const std::string QUEUE_TEST_HOME =
	"/tmp/quickdc-autotest-queue-" + std::to_string((int) getpid());

static bool queue_test_setup()
{
#ifdef _WIN32
	if (_putenv_s("HOME", QUEUE_TEST_HOME.c_str()) != 0) return false;
#else
	if (setenv("HOME", QUEUE_TEST_HOME.c_str(), 1) != 0) return false;
#endif
	Samurai::IO::File::mkdir(QUEUE_TEST_HOME.c_str(), 0700);
	Samurai::IO::File::mkdir("~/.quickdc", 0700);
	Samurai::IO::File(QUICKDC_QUEUE_FILE).remove();
	return true;
}

namespace {

/* And it goes again when the process does, or a name of its own per process
   would leave a directory behind for every case that asked for one. */
struct QueueTestHome
{
	~QueueTestHome()
	{
		Samurai::IO::File(QUICKDC_QUEUE_FILE).remove();
		Samurai::IO::File(QUICKDC_QUEUE_FILE_NEW).remove();
		Samurai::IO::File::rmdir((QUEUE_TEST_HOME + "/.quickdc").c_str());
		Samurai::IO::File::rmdir(QUEUE_TEST_HOME.c_str());
	}
};

QueueTestHome queue_test_home;

}

EXO_TEST(nmdc_adcget_file_wire_format, {
	DC::SendADCGet cmd("file", "TTH/" QUEUE_TEST_TTH_A, 1048576, 262144);
	std::string wire(cmd.getRawCommand(), cmd.getSize());
	return wire == "$ADCGET file TTH/" QUEUE_TEST_TTH_A " 1048576 262144|";
});

EXO_TEST(nmdc_adcget_filelist_wire_format, {
	/* A length of -1 means "all of it", so it must not print unsigned. */
	DC::SendADCGet cmd("list", "/", 0, -1);
	std::string wire(cmd.getRawCommand(), cmd.getSize());
	return wire == "$ADCGET list / 0 -1|";
});

EXO_TEST(nmdc_adcget_large_offset, {
	/* Well past 32 bits, which is the whole reason for the 64 bit types. */
	DC::SendADCGet cmd("file", "TTH/" QUEUE_TEST_TTH_B, 5368709120ULL, 1048576);
	std::string wire(cmd.getRawCommand(), cmd.getSize());
	return wire == "$ADCGET file TTH/" QUEUE_TEST_TTH_B " 5368709120 1048576|";
});

/* ------------------------------------------------------------------------- *
 * NMDC $SR parsing
 *
 * 0x05 is the field separator. The path and the hub name may both contain
 * spaces, and the separator count varies between file/directory and
 * active/passive replies, so these cover each shape.
 * ------------------------------------------------------------------------- */

#define SR_SEP "\005"

/** Holds the decoded parts, so each test can stay a single expression. */
struct SRResult {
	std::string nick;
	std::string path;
	std::string tth;
	uint64_t size;
	size_t freeslots;
	bool ok;
};

static SRResult sr_parse(const char* line)
{
	SRResult out;
	out.size = 0;
	out.freeslots = 0;
	out.ok = DC::parseSearchResult(line, strlen(line),
		out.nick, out.path, out.size, out.freeslots, out.tth);
	return out;
}

EXO_TEST(nmdc_sr_active_file_with_hubname, {
	SRResult r = sr_parse("$SR alice share\\movies\\file.bin" SR_SEP
	                      "3145728 2/4" SR_SEP "A QuickDC hub (192.168.0.1:411)");
	return r.ok
		&& r.nick == "alice"
		&& r.path == "share\\movies\\file.bin"
		&& r.size == 3145728
		&& r.freeslots == 2
		&& r.tth.empty();
});

EXO_TEST(nmdc_sr_active_file_with_tth, {
	SRResult r = sr_parse("$SR bob dir\\big.iso" SR_SEP "5368709120 0/3" SR_SEP
	                      "TTH:" QUEUE_TEST_TTH_A " (10.0.0.5:1411)");
	/* A size past 32 bits has to survive, and the TTH is what makes the
	   result usable as a source at all. */
	return r.ok
		&& r.nick == "bob"
		&& r.path == "dir\\big.iso"
		&& r.size == 5368709120ULL
		&& r.freeslots == 0
		&& r.tth == QUEUE_TEST_TTH_A;
});

EXO_TEST(nmdc_sr_passive_file_has_trailing_nick, {
	/* A reply relayed by the hub appends the searcher's nick. */
	SRResult r = sr_parse("$SR carol files\\song.mp3" SR_SEP "4096 1/2" SR_SEP
	                      "TTH:" QUEUE_TEST_TTH_B " (1.2.3.4:411)" SR_SEP "myself");
	return r.ok
		&& r.nick == "carol"
		&& r.path == "files\\song.mp3"
		&& r.size == 4096
		&& r.freeslots == 1
		&& r.tth == QUEUE_TEST_TTH_B;
});

EXO_TEST(nmdc_sr_directory_has_no_size, {
	/* No 0x05 before the slots, because a directory has no size. */
	SRResult r = sr_parse("$SR dave share\\movies 3/4" SR_SEP "A hub (1.2.3.4:411)");
	return r.ok
		&& r.nick == "dave"
		&& r.path == "share\\movies"
		&& r.size == 0
		&& r.freeslots == 3
		&& r.tth.empty();
});

EXO_TEST(nmdc_sr_passive_directory, {
	SRResult r = sr_parse("$SR eve share\\stuff 1/1" SR_SEP "A hub (1.2.3.4:411)" SR_SEP "myself");
	return r.ok && r.path == "share\\stuff" && r.size == 0 && r.freeslots == 1;
});

EXO_TEST(nmdc_sr_spaces_in_path_and_hubname, {
	/* Both fields may contain spaces, which is why the trailer is the anchor. */
	SRResult r = sr_parse("$SR frank my folder\\some long name.bin" SR_SEP
	                      "100 2/2" SR_SEP "The Best Hub Ever (1.2.3.4:411)");
	return r.ok
		&& r.path == "my folder\\some long name.bin"
		&& r.size == 100
		&& r.freeslots == 2;
});

EXO_TEST(nmdc_sr_brackets_in_hubname, {
	/* Only the last '(' opens the address, so a bracketed hub name is fine. */
	SRResult r = sr_parse("$SR gina f.bin" SR_SEP "10 1/1" SR_SEP
	                      "My (cool) hub (1.2.3.4:411)");
	return r.ok && r.path == "f.bin" && r.size == 10 && r.freeslots == 1;
});

EXO_TEST(nmdc_sr_nick_with_spaces_in_path_only, {
	/* A single character nick still parses; the first space ends it. */
	SRResult r = sr_parse("$SR x a b c" SR_SEP "5 1/1" SR_SEP "h (1.2.3.4:411)");
	return r.ok && r.nick == "x" && r.path == "a b c" && r.freeslots == 1;
});

EXO_TEST(nmdc_sr_short_tth_is_ignored, {
	/* Not 39 characters, so it must not be offered as a hash. */
	SRResult r = sr_parse("$SR hank f.bin" SR_SEP "10 1/1" SR_SEP "TTH:TOOSHORT (1.2.3.4:411)");
	return r.ok && r.tth.empty() && r.size == 10 && r.freeslots == 1;
});

EXO_TEST(nmdc_sr_slot_counts, {
	/*
	 * Worth its own case: "{free}/{total}" cannot go through
	 * Convert::to_int32(), which reads "2/4" as -6 because it skips the slash
	 * as if it were leading padding and then keeps going.
	 */
	if (sr_parse("$SR a f" SR_SEP "1 0/3" SR_SEP "h (1.2.3.4:411)").freeslots != 0) return false;
	if (sr_parse("$SR a f" SR_SEP "1 2/4" SR_SEP "h (1.2.3.4:411)").freeslots != 2) return false;
	if (sr_parse("$SR a f" SR_SEP "1 12/40" SR_SEP "h (1.2.3.4:411)").freeslots != 12) return false;

	/* A directory result carries slots too, in the same field as the path. */
	return sr_parse("$SR a some\\dir 7/9" SR_SEP "h (1.2.3.4:411)").freeslots == 7;
});

EXO_TEST(nmdc_sr_rejects_malformed, {
	/* No trailer at all. */
	if (sr_parse("$SR alice f.bin" SR_SEP "10 1/1" SR_SEP "A hub 1.2.3.4:411").ok) return false;

	/* Trailer without a colon is not an address, so nothing anchors the parse. */
	if (sr_parse("$SR alice f.bin" SR_SEP "10 1/1" SR_SEP "A hub (nocolon)").ok) return false;

	/* No slot field. */
	if (sr_parse("$SR alice f.bin" SR_SEP "10 xx" SR_SEP "h (1.2.3.4:411)").ok) return false;

	/* Slot counts that are not numbers. */
	if (sr_parse("$SR alice f.bin" SR_SEP "10 x/4" SR_SEP "h (1.2.3.4:411)").ok) return false;
	if (sr_parse("$SR alice f.bin" SR_SEP "10 2/x" SR_SEP "h (1.2.3.4:411)").ok) return false;
	if (sr_parse("$SR alice f.bin" SR_SEP "10 /4" SR_SEP "h (1.2.3.4:411)").ok) return false;

	/* A size that is not a number. */
	if (sr_parse("$SR alice f.bin" SR_SEP "12x 1/1" SR_SEP "h (1.2.3.4:411)").ok) return false;

	/* No nick. */
	if (sr_parse("$SR  f.bin" SR_SEP "10 1/1" SR_SEP "h (1.2.3.4:411)").ok) return false;

	/* Not a $SR at all. */
	if (sr_parse("$Search alice f.bin 10 1/1 h (1.2.3.4:411)").ok) return false;

	/* Too short to be anything. */
	if (sr_parse("$SR a b").ok) return false;
	if (sr_parse("").ok) return false;

	/* Only the hub trailer, so there is no path. */
	return !sr_parse("$SR alice " SR_SEP "h (1.2.3.4:411)").ok;
});

EXO_TEST(nmdc_sr_result_becomes_a_source, {
	/*
	 * The end to end point of parsing $SR: a reply whose TTH matches something
	 * queued has to be usable as a source, keyed on the nick since NMDC has
	 * no CID.
	 */
	if (!queue_test_setup()) return false;

	QuickDC::QueueManager queue;
	QuickDC::QueueFile* file = queue.add("/tmp/dl/big.iso", QUEUE_TEST_TTH_A, 0);
	if (file->countSources() != 0) return false;

	SRResult r = sr_parse("$SR bob dir\\big.iso" SR_SEP "5368709120 0/3" SR_SEP
	                      "TTH:" QUEUE_TEST_TTH_A " (10.0.0.5:1411)");
	if (!r.ok) return false;

	QuickDC::QueueFile* matched = queue.getFileByTTH(r.tth.c_str());
	if (matched != file) return false;

	/* The reply is also where an unknown size comes from. */
	if (!matched->getSize()) matched->setSize(r.size);
	if (matched->getSize() != 5368709120ULL) return false;

	matched->addSource(new QuickDC::FileSource("dchub://myhub:411", r.nick.c_str(), r.path.c_str()));
	if (matched->countSources() != 1) return false;

	QuickDC::FileSource* source = matched->findSource("dchub://myhub:411", "bob");
	if (!source) return false;

	/* And it round-trips through the queue on disk. */
	char* url = source->toURL();
	bool ok = (url && !strcmp(url, "dchub://myhub:411/bob/dir\\big.iso"));
	free(url);
	return ok;
});
