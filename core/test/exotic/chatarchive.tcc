/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The chat transcripts: what is written down, and what comes back.
 *
 * All of the risk is at the end of the file. A transcript is only ever appended
 * to and only ever read backwards, so the tail reader is what has to be right -
 * it walks the file in chunks and has to join a record that lies across two of
 * them.
 */

#include "quickdc.h"
#include "chat/archive.h"

#include <samurai/io/file.h>

#include <stdio.h>
#include <string.h>
#include <string>
#include <samurai/posix.h>
#include <vector>

/* A scratch directory of its own per process; see the note in queuecomplete. */
static const std::string ARCHIVE_HOME =
	"/tmp/quickdc_chat_" + std::to_string((int) getpid());

#define A_HUB "adc://hub.example.org:412"

namespace {

/*
 * The scratch directory goes when the process does. A name of its own per
 * process is what keeps two cases apart, and it is also what would leave one
 * behind for every case in the file - and a destructor is what runs however a
 * case returned.
 */
struct ArchiveHome
{
	~ArchiveHome() { Samurai::IO::File::rmdir(ARCHIVE_HOME.c_str()); }
};

ArchiveHome archive_home;

}

static QuickDC::ChatArchive::Line said(const char* nick, const char* text)
{
	QuickDC::ChatArchive::Line line;
	line.time = 1770000000;
	line.nick = nick;
	line.source = nick;
	line.text = text;
	return line;
}

EXO_TEST(chatarchive_a_line_comes_back_as_it_went_in,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	EXO_ASSERT(archive.isUsable());
	archive.forget(A_HUB, "");

	QuickDC::ChatArchive::Line sent = said("alice", "hello there  friend");
	sent.op = true;
	sent.action = true;
	sent.richText = true;
	sent.privateMessage = true;

	archive.append(A_HUB, "", sent);

	const std::vector<QuickDC::ChatArchive::Line> back = archive.tail(A_HUB, "", 10);
	EXO_ASSERT(back.size() == 1);

	EXO_ASSERT(back[0].nick == "alice");
	EXO_ASSERT(back[0].source == "alice");
	EXO_ASSERT(back[0].time == 1770000000);

	/* Two spaces in a row, which is where an escape that drops one would show. */
	EXO_ASSERT(back[0].text == "hello there  friend");

	EXO_ASSERT(back[0].op);
	EXO_ASSERT(back[0].action);
	EXO_ASSERT(back[0].richText);
	EXO_ASSERT(back[0].privateMessage);

	archive.forget(A_HUB, "");
	return 1;
});

/* Whatever somebody typed has to survive being a line in a line based format. */
EXO_TEST(chatarchive_the_awkward_characters_survive,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	const char* awkward = "a\\b c\nd\re\\sf\\\\g  h";
	archive.append(A_HUB, "", said("alice", awkward));

	const std::vector<QuickDC::ChatArchive::Line> back = archive.tail(A_HUB, "", 10);

	EXO_ASSERT(back.size() == 1);
	EXO_ASSERT(back[0].text == awkward);

	archive.forget(A_HUB, "");
	return 1;
});

/* A newline in a message must not read back as two messages. */
EXO_TEST(chatarchive_a_newline_does_not_become_a_second_line,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	archive.append(A_HUB, "", said("alice", "one\ntwo\nthree"));
	archive.append(A_HUB, "", said("bob", "after"));

	const std::vector<QuickDC::ChatArchive::Line> back = archive.tail(A_HUB, "", 10);

	EXO_ASSERT(back.size() == 2);
	EXO_ASSERT(back[0].text == "one\ntwo\nthree");
	EXO_ASSERT(back[1].text == "after");

	archive.forget(A_HUB, "");
	return 1;
});

EXO_TEST(chatarchive_conversations_are_kept_apart,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");
	archive.forget(A_HUB, "BOB");
	archive.forget("adc://elsewhere.example:412", "");

	archive.append(A_HUB, "", said("alice", "public"));
	archive.append(A_HUB, "BOB", said("bob", "private"));
	archive.append("adc://elsewhere.example:412", "", said("carol", "other"));

	EXO_ASSERT(archive.tail(A_HUB, "", 10).size() == 1);
	EXO_ASSERT(archive.tail(A_HUB, "", 10)[0].text == "public");
	EXO_ASSERT(archive.tail(A_HUB, "BOB", 10)[0].text == "private");
	EXO_ASSERT(archive.tail("adc://elsewhere.example:412", "", 10)[0].text == "other");

	archive.forget(A_HUB, "");
	archive.forget(A_HUB, "BOB");
	archive.forget("adc://elsewhere.example:412", "");
	return 1;
});

/*
 * The two halves of the key are joined before they are hashed, so a hub whose
 * address runs into a nickname cannot land on the file of another pairing that
 * happens to read the same way end to end.
 */
EXO_TEST(chatarchive_a_hub_and_a_peer_do_not_run_together,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);

	EXO_ASSERT(archive.pathFor("adc://a:412", "bc")
		!= archive.pathFor("adc://a:412b", "c"));
	return 1;
});

EXO_TEST(chatarchive_the_tail_is_the_recent_end_oldest_first,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	for (int n = 0; n < 50; n++)
		archive.append(A_HUB, "", said("alice", std::to_string(n).c_str()));

	const std::vector<QuickDC::ChatArchive::Line> back = archive.tail(A_HUB, "", 5);

	EXO_ASSERT(back.size() == 5);
	EXO_ASSERT(back.front().text == "45");
	EXO_ASSERT(back.back().text == "49");

	/* And asking for more than there is gives what there is. */
	EXO_ASSERT(archive.tail(A_HUB, "", 1000).size() == 50);

	archive.forget(A_HUB, "");
	return 1;
});

/*
 * The reader takes the file 64k at a time from the end, so a transcript past
 * that has records lying across the seam. This writes several chunks' worth
 * with long messages, so the join is exercised rather than assumed.
 */
EXO_TEST(chatarchive_records_spanning_a_read_chunk_are_rejoined,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	const std::string padding(700, 'x');
	const int total = 500;

	for (int n = 0; n < total; n++)
		archive.append(A_HUB, "", said("alice", (std::to_string(n) + ":" + padding).c_str()));

	const std::vector<QuickDC::ChatArchive::Line> back =
		archive.tail(A_HUB, "", (size_t) total);

	EXO_ASSERT(back.size() == (size_t) total);

	for (int n = 0; n < total; n++)
	{
		if (back[n].text != std::to_string(n) + ":" + padding)
		{
			printf("    record %d came back as '%s'\n", n, back[n].text.c_str());
			EXO_FAIL("a record did not survive the chunk boundary");
		}
	}

	archive.forget(A_HUB, "");
	return 1;
});

/* What a process that went down mid-write leaves behind. */
EXO_TEST(chatarchive_a_half_written_record_is_dropped,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	archive.append(A_HUB, "", said("alice", "first"));
	archive.append(A_HUB, "", said("alice", "second"));

	Samurai::IO::File file(archive.pathFor(A_HUB, ""));
	EXO_ASSERT(file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Append));
	const char* cut = "T1770000000 NIalice MShalf";
	file.write(cut, strlen(cut));
	file.close();

	/*
	 * Three records come back, the last of them the unterminated one - it is a
	 * whole record as far as anything can tell, and there is no way to know the
	 * writer had more to say. What must not happen is losing the two before it.
	 */
	const std::vector<QuickDC::ChatArchive::Line> back = archive.tail(A_HUB, "", 10);

	EXO_ASSERT(back.size() >= 2);
	EXO_ASSERT(back[0].text == "first");
	EXO_ASSERT(back[1].text == "second");

	archive.forget(A_HUB, "");
	return 1;
});

/* A line with no stamp is not a record, and does not take the file with it. */
EXO_TEST(chatarchive_a_line_that_is_not_a_record_is_skipped,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	archive.append(A_HUB, "", said("alice", "first"));

	Samurai::IO::File file(archive.pathFor(A_HUB, ""));
	EXO_ASSERT(file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Append));
	const char* rubbish = "not a record at all\n\n";
	file.write(rubbish, strlen(rubbish));
	file.close();

	archive.append(A_HUB, "", said("alice", "last"));

	const std::vector<QuickDC::ChatArchive::Line> back = archive.tail(A_HUB, "", 10);

	EXO_ASSERT(back.size() == 2);
	EXO_ASSERT(back[0].text == "first");
	EXO_ASSERT(back[1].text == "last");

	archive.forget(A_HUB, "");
	return 1;
});

/* A directory that cannot be made leaves an archive that quietly does nothing. */
EXO_TEST(chatarchive_an_unusable_directory_is_harmless,
{
	QuickDC::ChatArchive archive(std::string(""));

	EXO_ASSERT(!archive.isUsable());

	archive.append(A_HUB, "", said("alice", "hello"));

	EXO_ASSERT(archive.tail(A_HUB, "", 10).empty());
	EXO_ASSERT(!archive.contains(A_HUB, ""));
	archive.forget(A_HUB, "");
	return 1;
});

EXO_TEST(chatarchive_nothing_written_reads_as_nothing,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);

	EXO_ASSERT(!archive.contains(A_HUB, "nobody"));
	EXO_ASSERT(archive.tail(A_HUB, "nobody", 10).empty());

	/* And asking for none of it gives none of it rather than all of it. */
	archive.forget(A_HUB, "");
	archive.append(A_HUB, "", said("alice", "hello"));
	EXO_ASSERT(archive.tail(A_HUB, "", 0).empty());

	archive.forget(A_HUB, "");
	return 1;
});

EXO_TEST(chatarchive_forget_removes_the_transcript,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);

	archive.append(A_HUB, "BOB", said("bob", "hello"));
	EXO_ASSERT(archive.contains(A_HUB, "BOB"));

	archive.forget(A_HUB, "BOB");

	EXO_ASSERT(!archive.contains(A_HUB, "BOB"));
	EXO_ASSERT(archive.tail(A_HUB, "BOB", 10).empty());
	return 1;
});

/* ---- recognising a replay ---- */

/*
 * A hub replaying its history sends lines this client already has, stamped with
 * the times they were first said. Writing them again would double the record
 * every time the client reconnected.
 */
EXO_TEST(chatarchive_the_same_line_is_only_written_once,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	QuickDC::ChatArchive::Line line = said("alice", "hello");
	line.time = 1700000000;

	EXO_ASSERT(archive.append(A_HUB, "", line));
	EXO_ASSERT(!archive.append(A_HUB, "", line));
	EXO_ASSERT(!archive.append(A_HUB, "", line));

	EXO_ASSERT(archive.tail(A_HUB, "", 10).size() == 1);

	archive.forget(A_HUB, "");
	return 1;
});

/* Anything that differs is a different line, however slightly. */
EXO_TEST(chatarchive_a_line_that_differs_is_kept,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	QuickDC::ChatArchive::Line line = said("alice", "hello");
	line.time = 1700000000;
	EXO_ASSERT(archive.append(A_HUB, "", line));

	/* A second later. */
	QuickDC::ChatArchive::Line later = line;
	later.time = 1700000001;
	EXO_ASSERT(archive.append(A_HUB, "", later));

	/* The same second, different words. */
	QuickDC::ChatArchive::Line other = line;
	other.text = "hello?";
	EXO_ASSERT(archive.append(A_HUB, "", other));

	/* The same second and words, somebody else. */
	QuickDC::ChatArchive::Line bob = line;
	bob.nick = "bob";
	bob.source = "bob";
	EXO_ASSERT(archive.append(A_HUB, "", bob));

	EXO_ASSERT(archive.tail(A_HUB, "", 10).size() == 4);

	archive.forget(A_HUB, "");
	return 1;
});

/*
 * A replay after a gap: the hub hands back what was already recorded and the
 * lines said while this client was away. Only the second lot is new.
 */
EXO_TEST(chatarchive_a_replay_keeps_only_what_was_missed,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	/* What this client saw before it went. */
	for (int n = 0; n < 5; n++)
	{
		QuickDC::ChatArchive::Line line = said("alice", std::to_string(n).c_str());
		line.time = 1700000000 + n;
		EXO_ASSERT(archive.append(A_HUB, "", line));
	}

	/* What the hub replays on the way back in: all ten, five of them known. */
	int written = 0;

	for (int n = 0; n < 10; n++)
	{
		QuickDC::ChatArchive::Line line = said("alice", std::to_string(n).c_str());
		line.time = 1700000000 + n;
		if (archive.append(A_HUB, "", line)) written++;
	}

	EXO_ASSERT(written == 5);

	const std::vector<QuickDC::ChatArchive::Line> back = archive.tail(A_HUB, "", 100);
	EXO_ASSERT(back.size() == 10);

	/* And in the order they were said, not the order they were written. */
	for (size_t n = 0; n < back.size(); n++)
		EXO_ASSERT(back[n].text == std::to_string(n));

	archive.forget(A_HUB, "");
	return 1;
});

/* What was recorded before this archive was constructed counts as known. */
EXO_TEST(chatarchive_a_replay_is_recognised_across_a_restart,
{
	{
		QuickDC::ChatArchive archive(ARCHIVE_HOME);
		archive.forget(A_HUB, "");

		QuickDC::ChatArchive::Line line = said("alice", "hello");
		line.time = 1700000000;
		EXO_ASSERT(archive.append(A_HUB, "", line));
	}

	/* A second run of the client, reading the same file. */
	QuickDC::ChatArchive archive(ARCHIVE_HOME);

	QuickDC::ChatArchive::Line line = said("alice", "hello");
	line.time = 1700000000;

	EXO_ASSERT(!archive.append(A_HUB, "", line));
	EXO_ASSERT(archive.tail(A_HUB, "", 10).size() == 1);

	archive.forget(A_HUB, "");
	return 1;
});

/* Live chat is never compared: it is newer than anything already written. */
EXO_TEST(chatarchive_repeating_yourself_later_is_not_a_replay,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	QuickDC::ChatArchive::Line line = said("alice", "hello");
	line.time = 1700000000;
	EXO_ASSERT(archive.append(A_HUB, "", line));

	/* The same words again, a minute on. Said twice, so kept twice. */
	line.time = 1700000060;
	EXO_ASSERT(archive.append(A_HUB, "", line));

	EXO_ASSERT(archive.tail(A_HUB, "", 10).size() == 2);

	archive.forget(A_HUB, "");
	return 1;
});

/* Forgetting a conversation forgets what was known about it, too. */
EXO_TEST(chatarchive_a_forgotten_transcript_can_be_written_again,
{
	QuickDC::ChatArchive archive(ARCHIVE_HOME);
	archive.forget(A_HUB, "");

	QuickDC::ChatArchive::Line line = said("alice", "hello");
	line.time = 1700000000;

	EXO_ASSERT(archive.append(A_HUB, "", line));
	archive.forget(A_HUB, "");

	EXO_ASSERT(archive.append(A_HUB, "", line));
	EXO_ASSERT(archive.tail(A_HUB, "", 10).size() == 1);

	archive.forget(A_HUB, "");
	return 1;
});
