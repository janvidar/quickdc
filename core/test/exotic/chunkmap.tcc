/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * ChunkMap is what decides which part of a partial file to ask the next source
 * for, and which parts are already ours. Everything it hands out ends up as the
 * offset and length of a real transfer, and everything it accepts comes either
 * from the queue file on disk or, through fromIndexString(), from the PI field
 * of a PSR another peer sent us.
 *
 * The header states three invariants - completed ranges sorted, non-overlapping
 * and coalesced, so a complete file is exactly one range - and much of what
 * follows is checking that they still hold after operations that could break
 * them. check() asserts all three at once.
 */

#include "quickdc.h"
#include "network/chunkmap.h"

#include <stdio.h>
#include <string>


using ChunkMap = QuickDC::ChunkMap;

/* Sorted, non-overlapping, non-adjacent, and inside the file. */
static bool holds(const ChunkMap& map)
{
	uint64_t last = 0;
	for (size_t n = 0; n < map.getRangeCount(); n++)
	{
		uint64_t start = 0, end = 0;
		if (!map.getRange(n, start, end)) return false;

		if (end <= start) return false;
		if (n && start <= last) return false;   /* sorted, and a gap between */
		if (map.getSize() && end > map.getSize()) return false;

		last = end;
	}
	return true;
}

/* Expands to an assertion, so a map that does not hold together fails the
   case it was checked in. */
#define check(map) EXO_ASSERT(holds(map))

/* "0-100,200-300" as one string, for readable comparisons. */
static std::string ranges(const ChunkMap& map)
{
	return map.toString();
}

/* Nothing downloaded yet. */
EXO_TEST(chunkmap_an_empty_map_holds_nothing,
{
		ChunkMap map(1000);
		EXO_ASSERT(map.getSize() == 1000);
		EXO_ASSERT(map.getRangeCount() == 0);
		EXO_ASSERT(map.getDoneBytes() == 0);
		EXO_ASSERT(!map.isComplete());
		EXO_ASSERT(map.toString().empty());

		uint64_t start = 0, end = 0;
		EXO_ASSERT(!map.getRange(0, start, end));
	
	return 1;
});

/* A file whose size nobody has told us is never complete. */
EXO_TEST(chunkmap_a_file_of_unknown_size_is_never_complete,
{
		ChunkMap map;
		map.addRange(0, 1000);
		EXO_ASSERT(map.getDoneBytes() == 1000);
		EXO_ASSERT(!map.isComplete());
	
	return 1;
});

/* Ranges that touch are one range, or a complete file is not complete. */
EXO_TEST(chunkmap_adjacent_ranges_coalesce,
{
		ChunkMap map(2048);
		map.addRange(0, 1024);
		map.addRange(1024, 2048);

		EXO_ASSERT(map.getRangeCount() == 1);
		EXO_ASSERT(ranges(map) == "0-2048");
		EXO_ASSERT(map.isComplete());
		check(map);
	
	return 1;
});

/* Whichever order they arrive in. */
EXO_TEST(chunkmap_they_coalesce_backwards_too,
{
		ChunkMap map(2048);
		map.addRange(1024, 2048);
		map.addRange(0, 1024);

		EXO_ASSERT(map.getRangeCount() == 1);
		EXO_ASSERT(ranges(map) == "0-2048");
		check(map);
	
	return 1;
});

/* Overlapping ranges merge rather than being counted twice. */
EXO_TEST(chunkmap_overlapping_ranges_are_not_counted_twice,
{
		ChunkMap map(1000);
		map.addRange(0, 500);
		map.addRange(400, 800);

		EXO_ASSERT(map.getRangeCount() == 1);
		EXO_ASSERT(ranges(map) == "0-800");
		EXO_ASSERT(map.getDoneBytes() == 800);
		check(map);
	
	return 1;
});

/* One range bridging several swallows them all. */
EXO_TEST(chunkmap_a_range_bridging_several_merges_all_of_them,
{
		ChunkMap map(1000);
		map.addRange(0, 100);
		map.addRange(200, 300);
		map.addRange(400, 500);
		EXO_ASSERT(map.getRangeCount() == 3);

		map.addRange(50, 450);
		EXO_ASSERT(map.getRangeCount() == 1);
		EXO_ASSERT(ranges(map) == "0-500");
		check(map);
	
	return 1;
});

/* Adding what is already known changes nothing. */
EXO_TEST(chunkmap_adding_a_known_range_changes_nothing,
{
		ChunkMap map(1000);
		map.addRange(100, 200);
		const std::string before = ranges(map);

		map.addRange(100, 200);
		map.addRange(120, 180);
		EXO_ASSERT(ranges(map) == before);
		EXO_ASSERT(map.getDoneBytes() == 100);
		check(map);
	
	return 1;
});

/* Out of order insertions still come out sorted. */
EXO_TEST(chunkmap_ranges_are_kept_sorted,
{
		ChunkMap map(1000);
		map.addRange(600, 700);
		map.addRange(100, 200);
		map.addRange(400, 500);

		EXO_ASSERT(ranges(map) == "100-200,400-500,600-700");
		check(map);
	
	return 1;
});

/* An empty or reversed range is not a range. */
EXO_TEST(chunkmap_an_empty_or_reversed_range_is_ignored,
{
		ChunkMap map(1000);
		map.addRange(100, 100);
		map.addRange(300, 200);
		EXO_ASSERT(map.getRangeCount() == 0);
		EXO_ASSERT(map.getDoneBytes() == 0);
	
	return 1;
});

/* Nothing may be recorded past the end of the file. */
EXO_TEST(chunkmap_a_range_past_the_end_of_the_file_is_clamped,
{
		ChunkMap map(1000);
		map.addRange(900, 5000);
		EXO_ASSERT(ranges(map) == "900-1000");
		EXO_ASSERT(map.getDoneBytes() == 100);
		check(map);
	
	return 1;
});

/* Being told the size afterwards discards what no longer fits. */
EXO_TEST(chunkmap_setting_the_size_discards_what_is_outside_it,
{
		ChunkMap map;
		map.addRange(0, 100);
		map.addRange(500, 900);

		map.setSize(600);
		EXO_ASSERT(ranges(map) == "0-100,500-600");
		EXO_ASSERT(map.getSize() == 600);
		check(map);

		map.setSize(200);
		EXO_ASSERT(ranges(map) == "0-100");
		check(map);
	
	return 1;
});

/* Complete means one range covering all of it, and nothing less. */
EXO_TEST(chunkmap_a_file_with_a_hole_in_it_is_not_complete,
{
		ChunkMap map(1000);
		map.addRange(0, 999);
		EXO_ASSERT(!map.isComplete());

		map.addRange(999, 1000);
		EXO_ASSERT(map.isComplete());
		EXO_ASSERT(map.getRangeCount() == 1);

		ChunkMap gap(1000);
		gap.addRange(0, 400);
		gap.addRange(500, 1000);
		EXO_ASSERT(!gap.isComplete());
		EXO_ASSERT(gap.getDoneBytes() == 900);
	
	return 1;
});

/* How far the run holding an offset reaches. */
EXO_TEST(chunkmap_getrunend_reports_the_end_of_the_run_it_is_inside,
{
		ChunkMap map(1000);
		map.addRange(100, 200);
		map.addRange(300, 400);

		EXO_ASSERT(map.getRunEnd(100) == 200);
		EXO_ASSERT(map.getRunEnd(150) == 200);
		EXO_ASSERT(map.getRunEnd(199) == 200);

		/* Not downloaded, so it reaches nowhere. */
		EXO_ASSERT(map.getRunEnd(200) == 200);
		EXO_ASSERT(map.getRunEnd(0) == 0);
		EXO_ASSERT(map.getRunEnd(350) == 400);
	
	return 1;
});

/* Failing the hash means forgetting the lot. */
EXO_TEST(chunkmap_clearing_forgets_the_ranges_but_not_the_size,
{
		ChunkMap map(1000);
		map.addRange(0, 1000);
		EXO_ASSERT(map.isComplete());

		map.clear();
		EXO_ASSERT(map.getRangeCount() == 0);
		EXO_ASSERT(map.getDoneBytes() == 0);
		EXO_ASSERT(!map.isComplete());
		EXO_ASSERT(map.getSize() == 1000);
	
	return 1;
});

/* The first free part of the file, no bigger than asked for. */
EXO_TEST(chunkmap_reservations_are_handed_out_in_order_and_do_not_overlap,
{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.reserve(1024, start, length));
		EXO_ASSERT(start == 0);
		EXO_ASSERT(length == 1024);
		EXO_ASSERT(map.getReservationCount() == 1);

		/* And the next one starts where that ended. */
		EXO_ASSERT(map.reserve(1024, start, length));
		EXO_ASSERT(start == 1024);
		EXO_ASSERT(length == 1024);
		EXO_ASSERT(map.getReservationCount() == 2);
	
	return 1;
});

/* Downloaded parts are skipped, and a gap is not exceeded. */
EXO_TEST(chunkmap_a_reservation_fills_the_hole_exactly_and_no_more,
{
		ChunkMap map(10000);
		map.addRange(0, 1000);
		map.addRange(1500, 10000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.reserve(4096, start, length));
		EXO_ASSERT(start == 1000);
		EXO_ASSERT(length == 500);

		/* That was the only hole. */
		EXO_ASSERT(!map.hasFreeRange());
		EXO_ASSERT(!map.reserve(4096, start, length));
	
	return 1;
});

/* Nothing to hand out. */
EXO_TEST(chunkmap_nothing_is_reserved_when_there_is_nothing_to_reserve,
{
		ChunkMap done(1000);
		done.addRange(0, 1000);
		uint64_t start = 0, length = 0;
		EXO_ASSERT(!done.hasFreeRange());
		EXO_ASSERT(!done.reserve(100, start, length));

		/* Neither without a size, nor for a chunk of nothing. */
		ChunkMap unknown;
		EXO_ASSERT(!unknown.reserve(100, start, length));
		ChunkMap map(1000);
		EXO_ASSERT(!map.reserve(0, start, length));
	
	return 1;
});

/* Asking whether there is anything left must not take it. */
EXO_TEST(chunkmap_hasfreerange_does_not_reserve_anything_itself,
{
		ChunkMap map(1000);
		EXO_ASSERT(map.hasFreeRange());
		EXO_ASSERT(map.getReservationCount() == 0);

		EXO_ASSERT(map.hasFreeRange());
		EXO_ASSERT(map.getRangeCount() == 0);
	
	return 1;
});

/* The reservation delivered. */
EXO_TEST(chunkmap_committing_a_reservation_promotes_it_to_downloaded,
{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.reserve(1000, start, length));

		map.commit(start, length);
		EXO_ASSERT(map.getReservationCount() == 0);
		EXO_ASSERT(ranges(map) == "0-1000");
		EXO_ASSERT(map.getDoneBytes() == 1000);
		check(map);
	
	return 1;
});

/*
	 * A peer is allowed to answer with less than it was asked for. What did
	 * arrive is kept, and the rest has to be free for someone else.
	 */
EXO_TEST(chunkmap_a_short_commit_hands_the_remainder_back,
{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.reserve(4000, start, length));
		EXO_ASSERT(length == 4000);

		map.commit(start, 1500);
		EXO_ASSERT(ranges(map) == "0-1500");
		EXO_ASSERT(map.getReservationCount() == 0);

		EXO_ASSERT(map.reserve(4000, start, length));
		EXO_ASSERT(start == 1500);
	
	return 1;
});

/* The reservation failed. */
EXO_TEST(chunkmap_releasing_a_reservation_keeps_nothing_and_frees_the_range,
{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.reserve(1000, start, length));
		map.release(start, length);

		EXO_ASSERT(map.getReservationCount() == 0);
		EXO_ASSERT(map.getRangeCount() == 0);

		/* And it is on offer again. */
		EXO_ASSERT(map.reserve(1000, start, length));
		EXO_ASSERT(start == 0);
	
	return 1;
});

/* One release frees one reservation, not its neighbours. */
EXO_TEST(chunkmap_releasing_one_reservation_leaves_the_others_alone,
{
		ChunkMap map(10000);

		uint64_t a = 0, b = 0, len = 0;
		EXO_ASSERT(map.reserve(1000, a, len));
		EXO_ASSERT(map.reserve(1000, b, len));
		EXO_ASSERT(map.getReservationCount() == 2);

		map.release(b, len);
		EXO_ASSERT(map.getReservationCount() == 1);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.reserve(1000, start, length));
		EXO_ASSERT(start == b);
	
	return 1;
});

/* Losing track of who has what. */
EXO_TEST(chunkmap_clearing_the_reservations_offers_the_file_again,
{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.reserve(1000, start, length));
		EXO_ASSERT(map.reserve(1000, start, length));

		map.clearReservations();
		EXO_ASSERT(map.getReservationCount() == 0);
		EXO_ASSERT(map.reserve(1000, start, length));
		EXO_ASSERT(start == 0);
	
	return 1;
});

/* Reserving and committing the whole file, a chunk at a time. */
EXO_TEST(chunkmap_a_file_reserved_and_committed_in_chunks_ends_as_one_range,
{
		ChunkMap map(10000);

		int rounds = 0;
		uint64_t start = 0, length = 0;
		while (map.reserve(1024, start, length) && rounds++ < 100)
			map.commit(start, length);

		EXO_ASSERT(map.isComplete());
		EXO_ASSERT(map.getRangeCount() == 1);
		EXO_ASSERT(map.getDoneBytes() == 10000);
		EXO_ASSERT(map.getReservationCount() == 0);
		check(map);
	
	return 1;
});

/* A source that only holds part of the file can only be asked for that. */
EXO_TEST(chunkmap_a_partial_source_is_only_asked_for_what_it_has,
{
		ChunkMap theirs(10000);
		theirs.addRange(5000, 7000);

		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.hasFreeRangeWithin(theirs));
		EXO_ASSERT(map.getReservationCount() == 0);

		EXO_ASSERT(map.reserveWithin(theirs, 4096, start, length));
		EXO_ASSERT(start == 5000);
		EXO_ASSERT(length == 2000);

		/* That was all they had. */
		EXO_ASSERT(!map.hasFreeRangeWithin(theirs));
		EXO_ASSERT(!map.reserveWithin(theirs, 4096, start, length));

		/* But the rest of the file is still there for someone else. */
		EXO_ASSERT(map.hasFreeRange());
		EXO_ASSERT(map.reserve(4096, start, length));
		EXO_ASSERT(start == 0);
		EXO_ASSERT(length == 4096);
	
	return 1;
});

/* What we already have, inside what they have, is skipped. */
EXO_TEST(chunkmap_a_partial_source_is_not_asked_for_what_we_already_have,
{
		ChunkMap theirs(10000);
		theirs.addRange(0, 8000);

		ChunkMap map(10000);
		map.addRange(0, 3000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(map.reserveWithin(theirs, 1000, start, length));
		EXO_ASSERT(start == 3000);
		EXO_ASSERT(length == 1000);

		/* And so is what is already in flight. */
		EXO_ASSERT(map.reserveWithin(theirs, 1000, start, length));
		EXO_ASSERT(start == 4000);
	
	return 1;
});

/* A source with nothing we need. */
EXO_TEST(chunkmap_a_source_holding_nothing_we_need_is_no_use,
{
		ChunkMap theirs(10000);
		theirs.addRange(0, 1000);

		ChunkMap map(10000);
		map.addRange(0, 1000);

		uint64_t start = 0, length = 0;
		EXO_ASSERT(!map.hasFreeRangeWithin(theirs));
		EXO_ASSERT(!map.reserveWithin(theirs, 1000, start, length));

		ChunkMap nothing(10000);
		EXO_ASSERT(!map.hasFreeRangeWithin(nothing));
	
	return 1;
});

/* The queue file notation. */
EXO_TEST(chunkmap_the_queue_notation_survives_a_round_trip,
{
		ChunkMap map(1048576);
		map.addRange(0, 262144);
		map.addRange(524288, 1048576);

		EXO_ASSERT(map.toString() == "0-262144,524288-1048576");

		ChunkMap back(1048576);
		EXO_ASSERT(back.fromString(map.toString().c_str()));
		EXO_ASSERT(back.toString() == map.toString());
		EXO_ASSERT(back.getDoneBytes() == map.getDoneBytes());
		check(back);
	
	return 1;
});

/* Nothing downloaded is an empty string, both ways. */
EXO_TEST(chunkmap_an_empty_map_encodes_and_decodes_as_nothing,
{
		ChunkMap map(1000);
		EXO_ASSERT(map.toString().empty());
		EXO_ASSERT(map.fromString(""));
		EXO_ASSERT(map.getRangeCount() == 0);
		EXO_ASSERT(map.fromString(0));
	
	return 1;
});

/* Reading replaces whatever was there. */
EXO_TEST(chunkmap_reading_a_map_replaces_the_previous_contents,
{
		ChunkMap map(1000);
		map.addRange(0, 500);
		EXO_ASSERT(map.fromString("600-700"));
		EXO_ASSERT(map.toString() == "600-700");
	
	return 1;
});

/* Overlapping input is coalesced as it is read. */
EXO_TEST(chunkmap_overlapping_input_is_coalesced_on_the_way_in,
{
		ChunkMap map(1000);
		EXO_ASSERT(map.fromString("400-600,0-500"));
		EXO_ASSERT(map.toString() == "0-600");
		check(map);
	
	return 1;
});

/*
	 * Malformed input leaves the map empty rather than half parsed, so a queue
	 * file somebody edited cannot turn into a map claiming parts we do not have.
	 */
EXO_TEST(chunkmap_malformed_queue_notation_is_refused_and_keeps_nothing,
{

		const char* bad[] = {
			"garbage",
			"0",
			"0-",
			"-",
			"-100",
			"0-0",          /* empty range */
			"100-50",       /* backwards */
			"0-100,",       /* trailing separator */
			",0-100",
			"0-100,,200-300",
			"0-100 200-300",
			"0-100;200-300",
			"0-100,200",
			"0x10-0x20",
			0
		};

		for (size_t n = 0; bad[n]; n++)
		{
			ChunkMap map(100000);
			map.addRange(0, 1000);

			if (map.fromString(bad[n]))
			{
				const std::string why = std::string("accepted \"") + bad[n]
					+ "\" as \"" + map.toString() + "\"";
				EXO_FAIL(why.c_str());
			}

			/* And nothing of it was kept. */
			EXO_ASSERT(map.getRangeCount() == 0);
		}
	
	return 1;
});

/*
	 * strtoull() takes a sign and negates modulo 2^64, so a stray minus turns
	 * into a range reaching the end of the file - which is to say a partial
	 * download claiming to be complete.
	 */
EXO_TEST(chunkmap_a_negative_bound_does_not_become_the_whole_file,
{
		ChunkMap map(100000);
		EXO_ASSERT(!map.fromString("0--5"));
		EXO_ASSERT(map.getRangeCount() == 0);
		EXO_ASSERT(!map.isComplete());
	
	return 1;
});

/* A number too big for the type is not a number we know. */
EXO_TEST(chunkmap_a_bound_that_does_not_fit_is_refused,
{
		ChunkMap map(100000);
		EXO_ASSERT(!map.fromString("0-99999999999999999999999999"));
		EXO_ASSERT(map.getRangeCount() == 0);
	
	return 1;
});

/* The PSR notation counts whole blocks, not bytes. */
EXO_TEST(chunkmap_the_block_notation_counts_whole_blocks,
{
		ChunkMap map(10000);
		map.addRange(0, 3000);

		/* Blocks 0 and 1 of 1000 are whole; the run ends before block 3. */
		EXO_ASSERT(map.countBlocks(1000) == 3);
		EXO_ASSERT(map.countBlockRuns(1000) == 1);
		EXO_ASSERT(map.toIndexString(1000) == "0,3");

		map.addRange(5000, 6000);
		EXO_ASSERT(map.countBlocks(1000) == 4);
		EXO_ASSERT(map.countBlockRuns(1000) == 2);
		EXO_ASSERT(map.toIndexString(1000) == "0,3,5,6");
	
	return 1;
});

/* Half a block is not a block. */
EXO_TEST(chunkmap_a_partly_downloaded_block_does_not_count,
{
		ChunkMap map(10000);
		map.addRange(0, 1500);

		EXO_ASSERT(map.countBlocks(1000) == 1);
		EXO_ASSERT(map.toIndexString(1000) == "0,1");
	
	return 1;
});

/* The last block is short when the file is not a whole number of them. */
EXO_TEST(chunkmap_a_short_last_block_is_a_whole_block_of_the_file,
{
		ChunkMap map(2500);
		map.addRange(0, 2500);
		EXO_ASSERT(map.isComplete());

		EXO_ASSERT(map.countBlocks(1000) == 3);
		EXO_ASSERT(map.toIndexString(1000) == "0,3");

		ChunkMap back(2500);
		EXO_ASSERT(back.fromIndexString("0,3", 1000));
		EXO_ASSERT(back.isComplete());
		EXO_ASSERT(back.toString() == "0-2500");
		check(back);
	
	return 1;
});

/* What a peer sends us in a PSR. */
EXO_TEST(chunkmap_the_block_notation_survives_a_round_trip,
{
		ChunkMap map(10000);
		EXO_ASSERT(map.fromIndexString("0,2,5,7", 1000));
		EXO_ASSERT(map.toString() == "0-2000,5000-7000");
		EXO_ASSERT(map.getDoneBytes() == 4000);
		check(map);

		EXO_ASSERT(map.toIndexString(1000) == "0,2,5,7");
	
	return 1;
});

/* Nothing at all is a valid thing for a peer to claim. */
EXO_TEST(chunkmap_a_peer_holding_nothing_decodes_as_nothing,
{
		ChunkMap map(10000);
		EXO_ASSERT(map.fromIndexString("", 1000));
		EXO_ASSERT(map.getRangeCount() == 0);
		EXO_ASSERT(map.toIndexString(1000).empty());
	
	return 1;
});

/*
	 * And what a peer sends that is not a PSR at all. This one arrives straight
	 * off the network, so every one of these is somebody else's input.
	 */
EXO_TEST(chunkmap_malformed_block_notation_is_refused_and_keeps_nothing,
{

		const char* bad[] = {
			"garbage",
			"0",
			"0,",
			",",
			"0,0",           /* empty run */
			"3,1",           /* backwards */
			"0,2,",
			"0,2,,5,7",
			"0,2 5,7",
			"0,2,5",         /* odd number of indices */
			"-1,2",
			"0,-1",
			"99999999999999999999,99999999999999999999999",
			0
		};

		for (size_t n = 0; bad[n]; n++)
		{
			ChunkMap map(10000);
			map.addRange(0, 1000);

			if (map.fromIndexString(bad[n], 1000))
			{
				const std::string why = std::string("accepted \"") + bad[n]
					+ "\" as \"" + map.toString() + "\"";
				EXO_FAIL(why.c_str());
			}
			EXO_ASSERT(map.getRangeCount() == 0);
		}
	
	return 1;
});

/* A block index beyond the end of the file is not ours to believe. */
EXO_TEST(chunkmap_blocks_past_the_end_of_the_file_are_refused,
{
		ChunkMap map(10000);
		EXO_ASSERT(!map.fromIndexString("20,30", 1000));
		EXO_ASSERT(map.getRangeCount() == 0);
	
	return 1;
});

/* A run overhanging the end is clamped to it rather than refused. */
EXO_TEST(chunkmap_a_run_overhanging_the_end_is_clamped_to_the_file,
{
		ChunkMap map(2500);
		EXO_ASSERT(map.fromIndexString("0,4", 1000));
		EXO_ASSERT(map.toString() == "0-2500");
		EXO_ASSERT(map.isComplete());
		check(map);
	
	return 1;
});

/* Without a block size there is nothing to count. */
EXO_TEST(chunkmap_a_block_size_of_zero_counts_nothing,
{
		ChunkMap map(10000);
		map.addRange(0, 10000);
		EXO_ASSERT(map.countBlocks(0) == 0);
		EXO_ASSERT(map.countBlockRuns(0) == 0);
		EXO_ASSERT(map.toIndexString(0).empty());
		EXO_ASSERT(!map.fromIndexString("0,2", 0));
	
	return 1;
});
