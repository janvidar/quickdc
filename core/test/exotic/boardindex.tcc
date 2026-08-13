/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The client side of a bulletin board on disk: the cached index of a board, and
 * the documents held by hash.
 *
 * Three things here are worth more than the rest. The duplicate rule, because a
 * resumed subscription delivers the last second of a board twice and a client that
 * cannot tell that from a withdrawal either shows a post twice or never removes
 * one. The cursor, because "the highest seen plus one" is the natural thing to
 * write and it silently loses every post the hub accepted in the same second as
 * the last one received. And the paths, because a board name is protocol text a
 * hub chose - ".." is a legal one - so every filename either side of this is
 * derived rather than taken.
 *
 * The store's half is the eviction handshake: choosing a document to discard is
 * safe at any moment, and carrying that out is a detach() that would free an
 * object an upload in flight is reading through. The cases below hold it to
 * choosing and marking, and to doing nothing else until it is told nobody is
 * reading.
 */

#include "quickdc.h"
#include "api/core.h"
#include "api/bulletins.h"
#include "bbs/boardindex.h"
#include "bbs/poststore.h"
#include "config/preferences.h"
#include "hash/hashmanager.h"
#include "share/sharemanager.h"

#include <samurai/io/file.h>

#include <chrono>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

/* A scratch directory of its own per process; see the note in queuecomplete. */
static const std::string BOARD_HOME =
	"/tmp/quickdc_board_" + std::to_string((int) getpid());

static const std::string INDEX_HOME = BOARD_HOME + "/index";

#define A_HUB "adc://hub.example.org:412"
#define B_HUB "adc://elsewhere.example:412"
#define A_BOARD "general"

namespace {

/*
 * The scratch directories go when the process does. A name of its own per process
 * is what keeps two runs apart, and it is also what would leave one behind for
 * every case in the file - and a destructor is what runs however a case returned.
 */
struct BoardHome
{
	BoardHome() { Samurai::IO::File::mkdir(BOARD_HOME.c_str(), 0700); }

	~BoardHome()
	{
		Samurai::IO::File::rmdir(INDEX_HOME.c_str());
		Samurai::IO::File::rmdir(BOARD_HOME.c_str());
	}
};

BoardHome board_home;

}

/* A distinct tiger tree root per number: 39 characters of upper case base32. */
static std::string root(unsigned n)
{
	static const char* base32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

	std::string out(39, 'A');

	for (int at = 38; at >= 0 && n; at--)
	{
		out[at] = base32[n % 32];
		n /= 32;
	}

	return out;
}

static QuickDC::BoardEntry posted(unsigned n, uint64_t timestamp)
{
	QuickDC::BoardEntry entry;
	entry.tth = root(n);
	entry.size = 412;
	entry.cid = "IPJJWEPPPLCA3PF2ZCRRYO4F2ZX2EV2JMW2KC3I";
	entry.nick = "janvidar";
	entry.thread = root(n);
	entry.subject = "Hub upgrade on Saturday";
	entry.timestamp = timestamp;
	return entry;
}

static QuickDC::Board described(const char* name, uint64_t oldest)
{
	QuickDC::Board board;
	board.name = name;
	board.permissions = QuickDC::Subscribe;
	board.maxSize = 262144;
	board.oldest = oldest;
	return board;
}

static bool writeFile(const std::string& path, const std::string& content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) return false;
	const bool ok = fwrite(content.data(), 1, content.size(), fh) == content.size();
	fclose(fh);
	return ok;
}

/* ---- the cached index ---- */

EXO_TEST(boardindex_an_entry_comes_back_as_it_went_in,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	EXO_ASSERT(index.isUsable());
	index.forget(A_HUB, A_BOARD);

	QuickDC::BoardEntry sent = posted(1, 1786439662);
	sent.parent = root(2);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, sent));

	const std::vector<QuickDC::BoardEntry> back = index.entries(A_HUB, A_BOARD);
	EXO_ASSERT(back.size() == 1);

	EXO_ASSERT(back[0].tth == root(1));
	EXO_ASSERT(back[0].size == 412);
	EXO_ASSERT(back[0].cid == "IPJJWEPPPLCA3PF2ZCRRYO4F2ZX2EV2JMW2KC3I");
	EXO_ASSERT(back[0].nick == "janvidar");
	EXO_ASSERT(back[0].parent == root(2));
	EXO_ASSERT(back[0].thread == root(1));
	EXO_ASSERT(back[0].subject == "Hub upgrade on Saturday");
	EXO_ASSERT(back[0].timestamp == 1786439662);
	EXO_ASSERT(!back[0].removed);

	/* The file is the board, so an entry reads back on the one it was written to. */
	EXO_ASSERT(back[0].board == A_BOARD);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/*
 * A subject and a nickname are remote text and reach a line based format, so
 * whatever somebody put in them has to survive being written down.
 */
EXO_TEST(boardindex_the_awkward_characters_survive,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	const char* awkward = "a\\b c\nd\re\\sf\\\\g  h";

	QuickDC::BoardEntry sent = posted(1, 1786439662);
	sent.subject = awkward;
	sent.nick = awkward;

	EXO_ASSERT(index.append(A_HUB, A_BOARD, sent));

	const std::vector<QuickDC::BoardEntry> back = index.entries(A_HUB, A_BOARD);

	EXO_ASSERT(back.size() == 1);
	EXO_ASSERT(back[0].subject == awkward);
	EXO_ASSERT(back[0].nick == awkward);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/*
 * The resumption window. A client resumes from the highest timestamp it has seen,
 * so the entries of that second arrive a second time, carrying the timestamps they
 * carried before.
 */
EXO_TEST(boardindex_the_same_entry_at_the_same_timestamp_is_discarded,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	const QuickDC::BoardEntry entry = posted(1, 1786439662);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, entry));
	EXO_ASSERT(!index.append(A_HUB, A_BOARD, entry));
	EXO_ASSERT(!index.append(A_HUB, A_BOARD, entry));

	EXO_ASSERT(index.entries(A_HUB, A_BOARD).size() == 1);

	/* And an entry the hub sends again with less to say than it had before does
	   not overwrite what is held with the smaller version of itself. */
	QuickDC::BoardEntry sparse = entry;
	sparse.subject.clear();
	EXO_ASSERT(!index.append(A_HUB, A_BOARD, sparse));
	EXO_ASSERT(index.entries(A_HUB, A_BOARD)[0].subject == "Hub upgrade on Saturday");

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/*
 * The other half of the same rule: a withdrawal arrives as the same hash with the
 * time of the withdrawal on it, which is greater than anything on the board.
 */
EXO_TEST(boardindex_a_greater_timestamp_replaces_what_is_held,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439662)));

	QuickDC::BoardEntry tombstone;
	tombstone.tth = root(1);
	tombstone.timestamp = 1786443100;
	tombstone.removed = true;

	EXO_ASSERT(index.append(A_HUB, A_BOARD, tombstone));

	/* One entry for the hash and not two, and it is the tombstone. */
	const std::vector<QuickDC::BoardEntry> back = index.entries(A_HUB, A_BOARD);

	EXO_ASSERT(back.size() == 1);
	EXO_ASSERT(back[0].tth == root(1));
	EXO_ASSERT(back[0].removed);
	EXO_ASSERT(back[0].timestamp == 1786443100);

	/* Which the cursor has moved to, so the tombstone is not replayed for ever. */
	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 1786443100);

	/* Sent again in the next resumption window, it is a duplicate like any other. */
	EXO_ASSERT(!index.append(A_HUB, A_BOARD, tombstone));

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* An entry whose hash is not a tiger tree root names no document and gets no
   record - nor a filename made out of whatever it says instead. */
EXO_TEST(boardindex_an_entry_that_names_no_document_is_refused,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	QuickDC::BoardEntry entry = posted(1, 1786439662);

	entry.tth = "../../etc/passwd";
	EXO_ASSERT(!index.append(A_HUB, A_BOARD, entry));

	entry.tth = "kx3tq7zvn5plqgkd3ndbk6ztzg5pyqxsnmfyvjh";
	EXO_ASSERT(!index.append(A_HUB, A_BOARD, entry));

	entry.tth.clear();
	EXO_ASSERT(!index.append(A_HUB, A_BOARD, entry));

	EXO_ASSERT(index.entries(A_HUB, A_BOARD).empty());
	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 0);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/*
 * The cursor is the highest timestamp seen and not one past it. Timestamps are
 * seconds and are not unique, so a client that resumes from one second later never
 * receives the posts the hub accepted in the same second as the last one it saw.
 */
EXO_TEST(boardindex_the_cursor_is_the_highest_seen_and_not_one_past_it,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 0);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439000)));
	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(2, 1786439662)));

	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 1786439662);

	/* An entry arriving out of order does not drag the cursor backwards. */
	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(3, 1786439100)));
	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 1786439662);

	/*
	 * And what the cursor is for: resuming from it delivers the entries of that
	 * second again, one of them a post that was accepted in the same second and
	 * never seen. A cursor of 1786439663 would have skipped it.
	 */
	EXO_ASSERT(!index.append(A_HUB, A_BOARD, posted(2, index.cursor(A_HUB, A_BOARD))));
	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(4, index.cursor(A_HUB, A_BOARD))));

	EXO_ASSERT(index.entries(A_HUB, A_BOARD).size() == 4);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* Read back in the order the hub put the board in, not the order the records
   were written. */
EXO_TEST(boardindex_the_entries_are_ordered_by_the_timestamp_the_hub_assigned,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439662)));
	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(2, 1786439000)));
	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(3, 1786439100)));

	const std::vector<QuickDC::BoardEntry> back = index.entries(A_HUB, A_BOARD);

	EXO_ASSERT(back.size() == 3);
	EXO_ASSERT(back[0].tth == root(2));
	EXO_ASSERT(back[1].tth == root(3));
	EXO_ASSERT(back[2].tth == root(1));

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* ---- the gap that cannot be closed ---- */

/*
 * A hub replays no further back than the OT of its descriptor. A stored cursor
 * older than that is a view of the board with a hole in it, and it is a state of
 * its own rather than a board known to be whole.
 */
EXO_TEST(boardindex_a_cursor_older_than_the_oldest_replay_is_a_gap,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	/* Nothing has said what the hub will replay, so the question has no answer. */
	EXO_ASSERT(index.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Unknown);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439662)));
	EXO_ASSERT(index.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Unknown);

	/* A hub that will replay everything it has, from before what is held. */
	index.describe(A_HUB, described(A_BOARD, 1767225600));
	EXO_ASSERT(index.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Complete);

	/* And the same hub, later, expiring past the cursor: the entries between the
	   two are not coming, and no subscription will close that. */
	index.describe(A_HUB, described(A_BOARD, 1786500000));
	EXO_ASSERT(index.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Gap);

	/* Which survives the client being restarted, since it is the stored cursor
	   against the stored OT that decides it. */
	{
		QuickDC::BoardIndex second(INDEX_HOME);
		EXO_ASSERT(second.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Gap);
		EXO_ASSERT(second.cursor(A_HUB, A_BOARD) == 1786439662);
	}

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/*
 * A board nobody has subscribed to yet, on a hub that expires entries: what will
 * be received begins in the middle, and calling that complete would be a client
 * that says it has a board it has only the end of.
 */
EXO_TEST(boardindex_a_board_never_read_from_a_hub_that_expires_has_a_gap,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	index.describe(A_HUB, described(A_BOARD, 1767225600));
	EXO_ASSERT(index.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Gap);

	/* A hub that will replay the whole board has nothing to hide from a client
	   that holds none of it. */
	index.forget(A_HUB, A_BOARD);
	index.describe(A_HUB, described(A_BOARD, 0));
	EXO_ASSERT(index.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Complete);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* The descriptor arrives again whenever any field of a board changes, and most of
   those changes say nothing about what is held here. */
EXO_TEST(boardindex_an_unchanged_oldest_timestamp_is_not_written_again,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	index.describe(A_HUB, described(A_BOARD, 1767225600));

	const off_t after = Samurai::IO::File(index.pathFor(A_HUB, A_BOARD)).size();

	for (int n = 0; n < 20; n++)
		index.describe(A_HUB, described(A_BOARD, 1767225600));

	EXO_ASSERT(Samurai::IO::File(index.pathFor(A_HUB, A_BOARD)).size() == after);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* ---- one hub's assertions about one board ---- */

/*
 * Two hubs with a board of the same name are two boards. The entries are one
 * hub's claims, and merged they would describe a board that exists nowhere.
 */
EXO_TEST(boardindex_two_hubs_with_one_board_name_are_kept_apart,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);
	index.forget(B_HUB, A_BOARD);
	index.forget(A_HUB, "announcements");

	EXO_ASSERT(index.pathFor(A_HUB, A_BOARD) != index.pathFor(B_HUB, A_BOARD));
	EXO_ASSERT(index.pathFor(A_HUB, A_BOARD) != index.pathFor(A_HUB, "announcements"));

	QuickDC::BoardEntry here = posted(1, 1786439662);
	here.nick = "here";

	/* The same post, indexed on both hubs: one document, two sets of assertions. */
	QuickDC::BoardEntry there = posted(1, 1786000000);
	there.nick = "there";

	EXO_ASSERT(index.append(A_HUB, A_BOARD, here));
	EXO_ASSERT(index.append(B_HUB, A_BOARD, there));
	EXO_ASSERT(index.append(A_HUB, "announcements", posted(2, 1786439000)));

	EXO_ASSERT(index.entries(A_HUB, A_BOARD).size() == 1);
	EXO_ASSERT(index.entries(A_HUB, A_BOARD)[0].nick == "here");
	EXO_ASSERT(index.entries(B_HUB, A_BOARD)[0].nick == "there");

	/* And a cursor is one hub's, so being up to date on one says nothing here. */
	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 1786439662);
	EXO_ASSERT(index.cursor(B_HUB, A_BOARD) == 1786000000);

	EXO_ASSERT(index.entries(A_HUB, "announcements").size() == 1);
	EXO_ASSERT(index.entries(A_HUB, "announcements")[0].tth == root(2));

	index.forget(A_HUB, A_BOARD);
	index.forget(B_HUB, A_BOARD);
	index.forget(A_HUB, "announcements");
	return 1;
});

/*
 * The two halves of the key are joined before they are hashed, so a hub whose
 * address runs into a board name cannot land on the file of another pairing that
 * reads the same way end to end.
 */
EXO_TEST(boardindex_a_hub_and_a_board_do_not_run_together,
{
	QuickDC::BoardIndex index(INDEX_HOME);

	EXO_ASSERT(index.pathFor("adc://a:412", "bc") != index.pathFor("adc://a:412b", "c"));
	return 1;
});

/*
 * The file says which key it was made for, so two keys landing on one name cost
 * the second its cache rather than handing it the first one's entries. A hub
 * picks its own board names, so without this it could aim one at another hub's
 * file - and this is what happens when it does.
 */
EXO_TEST(boardindex_a_file_belonging_to_another_key_is_left_alone,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	/* Somebody else's index, standing where this board's would be. */
	const std::string path = index.pathFor(A_HUB, A_BOARD);

	EXO_ASSERT(writeFile(path,
		std::string("K HU") + B_HUB + " BDsomewhere\\selse\n"
		"E TR" + root(9) + " TS1786439000 SIsomebody\\selse\n"));

	const off_t before = Samurai::IO::File(path).size();

	/* Nothing is read out of it, so no other hub's entry is reported as this
	   board's - and a live entry is still not called a duplicate. */
	QuickDC::BoardIndex reading(INDEX_HOME);

	EXO_ASSERT(reading.entries(A_HUB, A_BOARD).empty());
	EXO_ASSERT(reading.cursor(A_HUB, A_BOARD) == 0);
	EXO_ASSERT(reading.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Unknown);
	EXO_ASSERT(!reading.contains(A_HUB, A_BOARD));
	EXO_ASSERT(reading.append(A_HUB, A_BOARD, posted(1, 1786439662)));

	/* And nothing is written into it either, whichever way the collision fell. */
	reading.describe(A_HUB, described(A_BOARD, 1767225600));
	reading.forget(A_HUB, A_BOARD);

	EXO_ASSERT(Samurai::IO::File(path).size() == before);

	Samurai::IO::File(path).remove();
	return 1;
});

/* ---- names a hub chose ---- */

/*
 * A board name is protocol text, and the character set excludes the separators
 * but permits '.': ".." is a legal board name and so is "...". Every file here is
 * named after a digest, so what a hub calls a board never reaches a path.
 */
EXO_TEST(boardindex_a_hostile_board_name_does_not_reach_a_path,
{
	QuickDC::BoardIndex index(INDEX_HOME);

	const std::string nul("gene\0ral", 8);

	const char* names[] = { "..", "...", "../../etc/passwd", "/etc/passwd",
		".", "board/../../..", "" };

	for (const char* name : names)
	{
		const std::string path = index.pathFor(A_HUB, name);

		/* Inside the index directory, one component down, and named in
		   characters this client picked. */
		EXO_ASSERT(path.compare(0, INDEX_HOME.size() + 1, INDEX_HOME + "/") == 0);

		const std::string leaf = path.substr(INDEX_HOME.size() + 1);

		EXO_ASSERT(leaf.find('/') == std::string::npos);
		EXO_ASSERT(leaf.find("..") == std::string::npos);
		EXO_ASSERT(leaf.size() == 20);

		/* And it resolves to itself: nothing in it is a traversal. */
		EXO_ASSERT(Samurai::IO::File::resolvePath(path) == path);
	}

	/* A name with a NUL in it is one name, and its digest has none. */
	const std::string path = index.pathFor(A_HUB, nul);

	EXO_ASSERT(path.find('\0') == std::string::npos);
	EXO_ASSERT(path != index.pathFor(A_HUB, "gene"));
	EXO_ASSERT(path != index.pathFor(A_HUB, A_BOARD));
	EXO_ASSERT(index.pathFor(A_HUB, "..") != index.pathFor(A_HUB, "..."));

	/* Written to, it is a file in the index directory like any other. */
	QuickDC::BoardIndex writing(INDEX_HOME);
	writing.forget(A_HUB, "..");

	EXO_ASSERT(writing.append(A_HUB, "..", posted(1, 1786439662)));
	EXO_ASSERT(Samurai::IO::File(writing.pathFor(A_HUB, "..")).isRegular());
	EXO_ASSERT(writing.entries(A_HUB, "..").size() == 1);
	EXO_ASSERT(writing.entries(A_HUB, "..")[0].board == "..");

	/* Nothing was made in the directory above, which is where ".." points. */
	EXO_ASSERT(!Samurai::IO::File::exists((BOARD_HOME + "/..idx").c_str()));

	writing.forget(A_HUB, "..");
	return 1;
});

/* ---- what a process that went down leaves behind ---- */

EXO_TEST(boardindex_a_half_written_record_is_tolerated,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439000)));
	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(2, 1786439100)));

	Samurai::IO::File file(index.pathFor(A_HUB, A_BOARD));
	EXO_ASSERT(file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Append));
	/* Cut in the middle of the hash, which is where a record stops being one. */
	const std::string cut = "E TR" + root(3).substr(0, 20);
	file.write(cut.data(), cut.size());
	file.close();

	/*
	 * The truncated record is not a record - its hash is not a hash - and what
	 * must not happen is losing the two before it, or the file being taken for
	 * unreadable because its last line is.
	 */
	QuickDC::BoardIndex reading(INDEX_HOME);

	const std::vector<QuickDC::BoardEntry> back = reading.entries(A_HUB, A_BOARD);

	EXO_ASSERT(back.size() == 2);
	EXO_ASSERT(back[0].tth == root(1));
	EXO_ASSERT(back[1].tth == root(2));
	EXO_ASSERT(reading.cursor(A_HUB, A_BOARD) == 1786439100);

	/* And the next entry appends onto it rather than being lost with it. */
	EXO_ASSERT(reading.append(A_HUB, A_BOARD, posted(4, 1786439200)));
	EXO_ASSERT(reading.entries(A_HUB, A_BOARD).size() == 3);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* Garbage in the middle of a file costs its own line and nothing else. */
EXO_TEST(boardindex_a_line_that_is_not_a_record_is_skipped,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439000)));

	Samurai::IO::File file(index.pathFor(A_HUB, A_BOARD));
	EXO_ASSERT(file.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Append));
	const char* rubbish = "not a record at all\n\nZ nor this one\nE\n";
	file.write(rubbish, strlen(rubbish));
	file.close();

	QuickDC::BoardIndex reading(INDEX_HOME);
	EXO_ASSERT(reading.append(A_HUB, A_BOARD, posted(2, 1786439100)));

	const std::vector<QuickDC::BoardEntry> back = reading.entries(A_HUB, A_BOARD);

	EXO_ASSERT(back.size() == 2);
	EXO_ASSERT(back[0].tth == root(1));
	EXO_ASSERT(back[1].tth == root(2));

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/*
 * A resumed subscription arrives as a burst of thousands, so what is held is held
 * in memory: reading the file back to answer each one of them would be the whole
 * of a board read once per entry in it.
 *
 * Asserted as behaviour rather than as a time. The file is taken away once it has
 * been read, and every entry in it is still recognised as one already held - which
 * it could only be from memory, and which also shows that a duplicate is refused
 * before anything is written.
 */
EXO_TEST(boardindex_thousands_of_entries_are_not_read_back_one_at_a_time,
{
	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	const unsigned total = 3000;

	for (unsigned n = 1; n <= total; n++)
		EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(n, 1786439000 + n)));

	EXO_ASSERT(index.entries(A_HUB, A_BOARD).size() == total);
	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 1786439000 + total);

	const std::string path = index.pathFor(A_HUB, A_BOARD);
	EXO_ASSERT(Samurai::IO::File(path).remove());

	for (unsigned n = 1; n <= total; n++)
		EXO_ASSERT(!index.append(A_HUB, A_BOARD, posted(n, 1786439000 + n)));

	EXO_ASSERT(!Samurai::IO::File::exists(path.c_str()));

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* ---- forgetting, and having nowhere to write ---- */

EXO_TEST(boardindex_forget_removes_the_index,
{
	QuickDC::BoardIndex index(INDEX_HOME);

	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439662)));
	index.describe(A_HUB, described(A_BOARD, 1767225600));
	EXO_ASSERT(index.contains(A_HUB, A_BOARD));

	index.forget(A_HUB, A_BOARD);

	EXO_ASSERT(!index.contains(A_HUB, A_BOARD));
	EXO_ASSERT(index.entries(A_HUB, A_BOARD).empty());
	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 0);
	EXO_ASSERT(index.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Unknown);

	/* And what was thrown away is not still recognised as a duplicate. */
	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439662)));
	EXO_ASSERT(index.entries(A_HUB, A_BOARD).size() == 1);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* A directory that cannot be made leaves an index that quietly does nothing. */
EXO_TEST(boardindex_an_unusable_directory_is_harmless,
{
	QuickDC::BoardIndex index(std::string(""));

	EXO_ASSERT(!index.isUsable());

	/* True, because nothing is kept, so nothing can be a repeat of anything and
	   a caller must not be told to drop a live entry. */
	EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(1, 1786439662)));

	EXO_ASSERT(index.entries(A_HUB, A_BOARD).empty());
	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 0);
	EXO_ASSERT(!index.contains(A_HUB, A_BOARD));

	index.describe(A_HUB, described(A_BOARD, 1767225600));
	EXO_ASSERT(index.view(A_HUB, A_BOARD) == QuickDC::BoardIndex::View::Unknown);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

/* Nor does a file that a directory in the way stops from being made. */
EXO_TEST(boardindex_a_directory_that_cannot_be_made_is_reported,
{
	const std::string blocked = BOARD_HOME + "/blocked";

	EXO_ASSERT(writeFile(blocked, "not a directory"));

	QuickDC::BoardIndex index(blocked);
	EXO_ASSERT(!index.isUsable());

	QuickDC::PostStore store(blocked, 0);
	EXO_ASSERT(!store.isUsable());
	EXO_ASSERT(!store.adopt(root(1), blocked));

	Samurai::IO::File(blocked).remove();
	return 1;
});

/* ---- the documents ---- */

/*
 * A store of its own per case, cleaned up however the case returned. A document
 * that outlived its test would be adopted by the next one, which is the one thing
 * a store keyed by content cannot tell from a document it fetched itself.
 */
class Docs
{
	public:
		Docs()
			: directory(BOARD_HOME + "/docs_" + std::to_string(counter++))
		{
		}

		~Docs()
		{
			QuickDC::PostStore store(directory, 0);

			for (const std::string& tth : store.documents())
				Samurai::IO::File(store.pathFor(tth)).remove();

			Samurai::IO::File(store.journalPath()).remove();

			for (const std::string& path : made)
				Samurai::IO::File(path).remove();

			Samurai::IO::File::rmdir(directory.c_str());
		}

		Docs(const Docs&) = delete;
		Docs& operator=(const Docs&) = delete;

		/* A fetched document, as it lands before it is adopted. */
		std::string fetched(const char* leaf, const std::string& content)
		{
			const std::string path = BOARD_HOME + "/" + leaf;
			writeFile(path, content);
			made.push_back(path);
			return path;
		}

		std::string directory;
		std::vector<std::string> made;

		static int counter;
};

int Docs::counter = 0;

EXO_TEST(poststore_a_document_is_found_by_its_hash,
{
	Docs docs;
	QuickDC::PostStore store(docs.directory, 0);
	EXO_ASSERT(store.isUsable());

	const std::string document = "IBB0 ID" + root(7) + "\nthe body\n";
	const std::string path = docs.fetched("fetched.tmp", document);

	EXO_ASSERT(!store.has(root(1)));
	EXO_ASSERT(store.adopt(root(1), path));

	EXO_ASSERT(store.has(root(1)));
	EXO_ASSERT(store.pathFor(root(1)) == docs.directory + "/" + root(1));
	EXO_ASSERT(store.bytes() == document.size());
	EXO_ASSERT(store.documents().size() == 1);

	/* Moved rather than copied: the bytes are not on disk twice. */
	EXO_ASSERT(!Samurai::IO::File::exists(path.c_str()));
	EXO_ASSERT(Samurai::IO::File(store.pathFor(root(1))).size()
		== (off_t) document.size());

	/* Nothing has said whether these bytes are the post that hash names. */
	EXO_ASSERT(store.verification(root(1)) == QuickDC::PostStore::Verification::Unknown);

	return 1;
});

/*
 * The path is the hash and nothing else. A hash that is not a tiger tree root has
 * no path at all, which is what keeps a board name, a magnet's 'dn' and every
 * other thing a hub chose out of this directory.
 */
EXO_TEST(poststore_a_path_is_derived_from_the_hash_alone,
{
	Docs docs;
	QuickDC::PostStore store(docs.directory, 0);

	EXO_ASSERT(store.pathFor(root(1)) == docs.directory + "/" + root(1));

	const char* names[] = { "..", "../../etc/passwd", "/etc/passwd", "",
		"kx3tq7zvn5plqgkd3ndbk6ztzg5pyqxsnmfyvjh",
		"KX3TQ7ZVN5PLQGKD3NDBK6ZTZG5PYQXSNMFYVJ", "notes.pdf" };

	const std::string path = docs.fetched("hostile.tmp", "bytes");

	for (const char* name : names)
	{
		EXO_ASSERT(store.pathFor(name).empty());
		EXO_ASSERT(!store.adopt(name, path));
		EXO_ASSERT(!store.has(name));
	}

	/* The file the store refused is still the caller's. */
	EXO_ASSERT(Samurai::IO::File::exists(path.c_str()));

	return 1;
});

/* Verification is per document and global across hubs, so it is kept with the
   document and keyed by its hash - and it is not worth doing twice. */
EXO_TEST(poststore_the_verification_outcome_survives_a_reload,
{
	Docs docs;

	{
		QuickDC::PostStore store(docs.directory, 0);

		EXO_ASSERT(store.adopt(root(1), docs.fetched("one.tmp", "first document")));
		EXO_ASSERT(store.adopt(root(2), docs.fetched("two.tmp", "second document")));

		store.record(root(1), QuickDC::PostStore::Verification::Verified);
		store.record(root(2), QuickDC::PostStore::Verification::Failed);
	}

	/* A second run of the client, reading the same journal. */
	QuickDC::PostStore store(docs.directory, 0);

	EXO_ASSERT(store.verification(root(1)) == QuickDC::PostStore::Verification::Verified);
	EXO_ASSERT(store.verification(root(2)) == QuickDC::PostStore::Verification::Failed);
	EXO_ASSERT(store.verification(root(3)) == QuickDC::PostStore::Verification::Unknown);

	EXO_ASSERT(store.documents().size() == 2);
	EXO_ASSERT(store.documents()[0] == root(1));
	EXO_ASSERT(store.has(root(1)));

	return 1;
});

/* What is on disk is what is held. A document somebody removed by hand is not
   held, whatever the journal says about it. */
EXO_TEST(poststore_a_document_that_has_gone_from_disk_is_not_held,
{
	Docs docs;

	{
		QuickDC::PostStore store(docs.directory, 0);
		EXO_ASSERT(store.adopt(root(1), docs.fetched("one.tmp", "first document")));
		store.record(root(1), QuickDC::PostStore::Verification::Verified);

		EXO_ASSERT(Samurai::IO::File(store.pathFor(root(1))).remove());
	}

	QuickDC::PostStore store(docs.directory, 0);

	EXO_ASSERT(!store.has(root(1)));
	EXO_ASSERT(store.documents().empty());
	EXO_ASSERT(store.bytes() == 0);
	EXO_ASSERT(store.verification(root(1)) == QuickDC::PostStore::Verification::Unknown);

	return 1;
});

/* A withdrawal marks, and the mark is a decision rather than a deletion: the
   bytes are there until somebody says nothing is reading them. */
EXO_TEST(poststore_forget_marks_and_release_deletes,
{
	Docs docs;
	QuickDC::PostStore store(docs.directory, 0);

	EXO_ASSERT(store.adopt(root(1), docs.fetched("one.tmp", "withdrawn document")));

	EXO_ASSERT(!store.release(root(1)));
	EXO_ASSERT(store.forget(root(1)));

	EXO_ASSERT(store.pending().size() == 1);
	EXO_ASSERT(store.pending()[0] == root(1));
	EXO_ASSERT(store.has(root(1)));
	EXO_ASSERT(Samurai::IO::File::exists(store.pathFor(root(1)).c_str()));

	/* Which survives a restart, and at a restart nothing is being served, so a
	   client can carry out every mark it finds without asking anybody. */
	const std::string path = store.pathFor(root(1));

	{
		QuickDC::PostStore second(docs.directory, 0);
		EXO_ASSERT(second.pending().size() == 1);
		EXO_ASSERT(second.release(root(1)));

		EXO_ASSERT(!second.has(root(1)));
		EXO_ASSERT(second.documents().empty());
	}

	EXO_ASSERT(!Samurai::IO::File::exists(path.c_str()));

	/* And the file's absence is the record: a third run does not offer it again. */
	QuickDC::PostStore third(docs.directory, 0);
	EXO_ASSERT(!third.has(root(1)));
	EXO_ASSERT(!third.forget(root(1)));

	return 1;
});

/*
 * Eviction chooses documents and never index entries. An entry is a few hundred
 * bytes and a board without its entries is not a board, while a document is as
 * large as its author made it.
 */
EXO_TEST(poststore_eviction_chooses_documents_before_entries,
{
	Docs docs;
	QuickDC::PostStore store(docs.directory, 0);

	QuickDC::BoardIndex index(INDEX_HOME);
	index.forget(A_HUB, A_BOARD);

	for (unsigned n = 1; n <= 5; n++)
	{
		EXO_ASSERT(index.append(A_HUB, A_BOARD, posted(n, 1786439000 + n)));
		EXO_ASSERT(store.adopt(root(n),
			docs.fetched(("doc" + std::to_string(n) + ".tmp").c_str(),
				std::string(1000, 'x'))));
	}

	const off_t entries = Samurai::IO::File(index.pathFor(A_HUB, A_BOARD)).size();

	QuickDC::PostStore::Bounds bounds;
	bounds.total = 2500;

	const std::vector<std::string> chosen = store.evict(bounds, 1786440000);

	/* The oldest adoptions, and only as many as it takes to fit. */
	EXO_ASSERT(chosen.size() == 3);
	EXO_ASSERT(chosen[0] == root(1));
	EXO_ASSERT(chosen[2] == root(3));

	/* Marked, and not yet gone: choosing is safe at any moment and doing is not. */
	EXO_ASSERT(store.bytes() == 5000);
	EXO_ASSERT(store.has(root(1)));

	for (const std::string& tth : chosen)
		EXO_ASSERT(store.release(tth));

	EXO_ASSERT(store.bytes() == 2000);
	EXO_ASSERT(store.documents().size() == 2);
	EXO_ASSERT(!store.has(root(1)));
	EXO_ASSERT(store.has(root(4)));

	/* And the board is exactly as it was: nothing was saved by trimming it. */
	EXO_ASSERT(Samurai::IO::File(index.pathFor(A_HUB, A_BOARD)).size() == entries);
	EXO_ASSERT(index.entries(A_HUB, A_BOARD).size() == 5);
	EXO_ASSERT(index.cursor(A_HUB, A_BOARD) == 1786439005);

	index.forget(A_HUB, A_BOARD);
	return 1;
});

EXO_TEST(poststore_eviction_by_age_takes_what_is_old_enough,
{
	Docs docs;
	QuickDC::PostStore store(docs.directory, 0);

	EXO_ASSERT(store.adopt(root(1), docs.fetched("one.tmp", "a document")));

	QuickDC::PostStore::Bounds bounds;
	bounds.age = 86400;

	/* Nothing is old enough yet, and nothing was marked. */
	EXO_ASSERT(store.evict(bounds, 0).empty());
	EXO_ASSERT(store.pending().empty());

	/* A bound of nothing at all leaves everything alone; a caller that wants no
	   documents held says so by evicting them. */
	QuickDC::PostStore::Bounds none;
	EXO_ASSERT(store.evict(none, 0).empty());

	const std::vector<std::string> chosen =
		store.evict(bounds, (uint64_t) time(0) + 86400);

	EXO_ASSERT(chosen.size() == 1);
	EXO_ASSERT(chosen[0] == root(1));

	/* Chosen once. A second pass does not choose it again, and the mark stands. */
	EXO_ASSERT(store.evict(bounds, (uint64_t) time(0) + 86400).empty());
	EXO_ASSERT(store.pending().size() == 1);

	return 1;
});

/* ---- offering a document to peers ---- */

/*
 * A core, a hash manager and a share manager of their own, because serving a
 * document is the share manager's business: the hash is computed on the worker
 * thread and reported on this one, and what a peer can fetch is what the manager
 * will answer for.
 */
class Served
{
	public:
		Served()
			: cfg(BOARD_HOME + "/served_" + std::to_string(counter) + ".conf")
			, ses(BOARD_HOME + "/served_" + std::to_string(counter) + ".session")
			, hashcache(BOARD_HOME + "/served_" + std::to_string(counter) + ".cache")
			, out(BOARD_HOME + "/served_" + std::to_string(counter++))
		{
			Samurai::IO::File::mkdir(out.c_str());

			writeFile(cfg, "[User]\nNickname=tester\n\n[Shares]\nTotal shares=0\n");

			core = std::make_unique<QuickDC::Core>(cfg, ses);

			hashes = std::make_unique<QuickDC::Hash::Manager>(hashcache.c_str());
			shares = new QuickDC::Share::Manager(core->config, hashes.get(), out.c_str());

			/* The core owns whatever it built; this one replaces it. */
			delete core->shares;
			core->shares = shares;

			shares->initialize();
		}

		~Served()
		{
			shares = 0;
			core.reset();
			hashes.reset();

			Samurai::IO::File(hashcache).remove();
			Samurai::IO::File(hashcache + ".new").remove();
			Samurai::IO::File(out + "/files.xml").remove();
			Samurai::IO::File(out + "/files.xml.bz2").remove();
			Samurai::IO::File::rmdir(out.c_str());
			Samurai::IO::File(cfg).remove();
			Samurai::IO::File(ses).remove();
		}

		Served(const Served&) = delete;
		Served& operator=(const Served&) = delete;

		/*
		 * Runs the hash worker out, as the main loop does. Nothing about an offer
		 * is known until then: the hash arrives on this thread, from process().
		 */
		bool drain()
		{
			const auto deadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(120);

			while (hashes->process() || hashes->isBusy())
			{
				if (std::chrono::steady_clock::now() > deadline) return false;
				usleep(1000);
			}

			hashes->process();
			return true;
		}

		/* Whether a peer asking for this hash would be answered. */
		bool answersFor(const std::string& tth)
		{
			return shares->getFileByTTH(tth.c_str()) != 0;
		}

		std::string cfg;
		std::string ses;
		std::string hashcache;
		std::string out;
		std::unique_ptr<QuickDC::Core> core;
		std::unique_ptr<QuickDC::Hash::Manager> hashes;
		QuickDC::Share::Manager* shares = 0;

		static int counter;
};

int Served::counter = 0;

/*
 * The hash of a file, learned the way the share manager learns one, so that a
 * document can be adopted under the name its own bytes give it. A test that filed
 * a document under an arbitrary hash could not then be served it.
 */
class Asked : public QuickDC::Share::AttachmentListener
{
	public:
		void EventAttachmentReady(const char*, const char* tth, uint64_t) override
		{
			hash = tth ? tth : "";
		}

		void EventAttachmentFailed(const char*, const char*) override {}

		std::string hash;
};

static std::string hashOf(Served& served, const std::string& path)
{
	Asked asked;

	if (!served.shares->attach(path.c_str(), &asked,
		QuickDC::Share::Manager::Persistence::Session)) return std::string();

	if (!served.drain()) return std::string();

	if (!asked.hash.empty()) served.shares->detach(asked.hash);

	return asked.hash;
}

/*
 * What makes a board durable: a post ends up held by everybody who read it, and
 * answering for it by hash discloses nothing the board did not.
 */
EXO_TEST(poststore_a_verified_document_is_offered_by_its_hash,
{
	Docs docs;
	Served served;

	const std::string path = docs.fetched("post.tmp",
		"IBB0 ID" + root(7) + "\nthe body of a post\n");

	const std::string tth = hashOf(served, path);
	EXO_ASSERT(tth.size() == 39);

	QuickDC::PostStore store(docs.directory, served.shares);

	EXO_ASSERT(store.adopt(tth, path));
	store.record(tth, QuickDC::PostStore::Verification::Verified);

	/* Refused by default: serving says "I have read this" to whoever asks. */
	EXO_ASSERT(!served.answersFor(tth));
	EXO_ASSERT(!store.isServed(tth));

	EXO_ASSERT(store.serve(tth));
	EXO_ASSERT(served.drain());

	EXO_ASSERT(store.isServed(tth));
	EXO_ASSERT(served.answersFor(tth));

	/* By hash and by nothing else: nothing is shared, so there is no file list
	   entry to find and no name for a search to match. */
	EXO_ASSERT(served.shares->getFiles() == 0);
	EXO_ASSERT(served.shares->getSharedSize() == 0);

	/* Nothing is remembered for the next start-up: whether a document is offered
	   at all is a decision the caller makes again every time. */
	EXO_ASSERT(served.core->config->peekNumber("Attachments", "Total", 0) == 0);

	EXO_ASSERT(store.withhold(tth));
	EXO_ASSERT(store.release(tth));
	return 1;
});

/* A document that has not verified is never offered, whatever the caller asks
   for: answering for it would spread a forgery under a checked name. */
EXO_TEST(poststore_an_unverified_document_is_never_offered,
{
	Docs docs;
	Served served;

	const std::string path = docs.fetched("post.tmp", "IBB0 ID" + root(7) + "\nbody\n");
	const std::string tth = hashOf(served, path);
	EXO_ASSERT(tth.size() == 39);

	QuickDC::PostStore store(docs.directory, served.shares);
	EXO_ASSERT(store.adopt(tth, path));

	EXO_ASSERT(!store.serve(tth));
	EXO_ASSERT(served.drain());
	EXO_ASSERT(!store.isServed(tth));
	EXO_ASSERT(!served.answersFor(tth));

	store.record(tth, QuickDC::PostStore::Verification::Failed);
	EXO_ASSERT(!store.serve(tth));
	EXO_ASSERT(!served.answersFor(tth));

	/* Verified, offered, and then found wanting: the offer is withdrawn, and it
	   waits for a release() like every other withdrawal. */
	store.record(tth, QuickDC::PostStore::Verification::Verified);
	EXO_ASSERT(store.serve(tth));
	EXO_ASSERT(served.drain());
	EXO_ASSERT(served.answersFor(tth));

	store.record(tth, QuickDC::PostStore::Verification::Failed);
	EXO_ASSERT(!store.isServed(tth));
	EXO_ASSERT(store.pending().size() == 1);
	EXO_ASSERT(served.answersFor(tth));

	EXO_ASSERT(store.release(tth));
	EXO_ASSERT(!served.answersFor(tth));

	/* The document is still held: it was the offer that was withdrawn. */
	EXO_ASSERT(store.has(tth));

	return 1;
});

/*
 * The eviction handshake. detach() takes back the very object an upload reads
 * through until its transfer ends, and neither the share manager nor this store
 * hears about transfers - so choosing a document to discard marks it, and the
 * caller that can see the transfers in flight says when.
 */
EXO_TEST(poststore_a_marked_document_is_not_detached_until_the_caller_vouches,
{
	Docs docs;
	Served served;

	const std::string path = docs.fetched("post.tmp",
		"IBB0 ID" + root(7) + "\n" + std::string(4000, 'b') + "\n");

	const std::string tth = hashOf(served, path);
	EXO_ASSERT(tth.size() == 39);

	QuickDC::PostStore store(docs.directory, served.shares);

	EXO_ASSERT(store.adopt(tth, path));
	store.record(tth, QuickDC::PostStore::Verification::Verified);
	EXO_ASSERT(store.serve(tth));
	EXO_ASSERT(served.drain());
	EXO_ASSERT(served.answersFor(tth));

	QuickDC::PostStore::Bounds bounds;
	bounds.total = 1;

	const std::vector<std::string> chosen = store.evict(bounds, (uint64_t) time(0));

	EXO_ASSERT(chosen.size() == 1);
	EXO_ASSERT(chosen[0] == tth);

	/*
	 * Chosen, and nothing else has happened. The file is there, the hash is still
	 * answered for, and a transfer that was reading it is reading it still.
	 */
	EXO_ASSERT(Samurai::IO::File::exists(store.pathFor(tth).c_str()));
	EXO_ASSERT(served.answersFor(tth));
	EXO_ASSERT(store.has(tth));

	/* Which does not change however many times eviction runs again. */
	EXO_ASSERT(store.evict(bounds, (uint64_t) time(0)).empty());
	EXO_ASSERT(served.answersFor(tth));

	/* Nor is it offered again while it is on its way out. */
	EXO_ASSERT(!store.serve(tth));

	/* The caller vouches, and only now is the hash handed back and the file gone. */
	EXO_ASSERT(store.release(tth));

	EXO_ASSERT(!served.answersFor(tth));
	EXO_ASSERT(!store.has(tth));
	EXO_ASSERT(!Samurai::IO::File::exists((docs.directory + "/" + tth).c_str()));

	return 1;
});

/*
 * A document filed under a hash its bytes do not have is not the post that hash
 * names, however it came to be there. The share manager hashes what it is handed,
 * so this is where that is found out - and what was offered under the wrong name
 * is taken back on the same turn of the loop it appeared on.
 */
EXO_TEST(poststore_a_document_that_is_not_what_it_claims_is_not_offered,
{
	Docs docs;
	Served served;

	const std::string path = docs.fetched("post.tmp", "not the post it claims to be");

	QuickDC::PostStore store(docs.directory, served.shares);

	/* A caller that said it verified when it had not, which is the case this
	   cannot be talked out of by trusting it. */
	EXO_ASSERT(store.adopt(root(11), path));
	store.record(root(11), QuickDC::PostStore::Verification::Verified);

	EXO_ASSERT(store.serve(root(11)));
	EXO_ASSERT(served.drain());

	EXO_ASSERT(!store.isServed(root(11)));
	EXO_ASSERT(!served.answersFor(root(11)));

	/* And it is recorded, so it is not fetched, checked and refused again. */
	EXO_ASSERT(store.verification(root(11))
		== QuickDC::PostStore::Verification::Failed);

	return 1;
});

/* A client with no share manager holds documents and answers for none of them,
   which is a client that reads a board without saying that it has. */
EXO_TEST(poststore_a_store_with_nothing_to_serve_with_holds_documents,
{
	Docs docs;
	QuickDC::PostStore store(docs.directory, 0);

	EXO_ASSERT(store.adopt(root(1), docs.fetched("one.tmp", "a document")));
	store.record(root(1), QuickDC::PostStore::Verification::Verified);

	EXO_ASSERT(store.has(root(1)));
	EXO_ASSERT(!store.serve(root(1)));
	EXO_ASSERT(!store.isServed(root(1)));
	EXO_ASSERT(!store.withhold(root(1)));

	return 1;
});

/* A document is immutable and is named by its own bytes, so the second copy of
   one is the same copy. */
EXO_TEST(poststore_adopting_a_document_twice_keeps_the_first,
{
	Docs docs;
	QuickDC::PostStore store(docs.directory, 0);

	EXO_ASSERT(store.adopt(root(1), docs.fetched("one.tmp", "a document")));
	store.record(root(1), QuickDC::PostStore::Verification::Verified);

	const std::string again = docs.fetched("again.tmp", "a document");

	EXO_ASSERT(store.adopt(root(1), again));

	EXO_ASSERT(store.documents().size() == 1);
	EXO_ASSERT(!Samurai::IO::File::exists(again.c_str()));

	/* And what was verified stays verified: nothing was replaced. */
	EXO_ASSERT(store.verification(root(1))
		== QuickDC::PostStore::Verification::Verified);

	return 1;
});
