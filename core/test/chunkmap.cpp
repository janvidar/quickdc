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

static int failures = 0;

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			failures++; \
		} \
	} while (0)

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

#define check(map) CHECK(holds(map))

/* "0-100,200-300" as one string, for readable comparisons. */
static std::string ranges(const ChunkMap& map)
{
	return map.toString();
}

int main()
{
	setbuf(stdout, 0);

	/* Nothing downloaded yet. */
	{
		ChunkMap map(1000);
		CHECK(map.getSize() == 1000);
		CHECK(map.getRangeCount() == 0);
		CHECK(map.getDoneBytes() == 0);
		CHECK(!map.isComplete());
		CHECK(map.toString().empty());

		uint64_t start = 0, end = 0;
		CHECK(!map.getRange(0, start, end));
		printf("  ok an empty map holds nothing\n");
	}

	/* A file whose size nobody has told us is never complete. */
	{
		ChunkMap map;
		map.addRange(0, 1000);
		CHECK(map.getDoneBytes() == 1000);
		CHECK(!map.isComplete());
		printf("  ok a file of unknown size is never complete\n");
	}

	/* Ranges that touch are one range, or a complete file is not complete. */
	{
		ChunkMap map(2048);
		map.addRange(0, 1024);
		map.addRange(1024, 2048);

		CHECK(map.getRangeCount() == 1);
		CHECK(ranges(map) == "0-2048");
		CHECK(map.isComplete());
		check(map);
		printf("  ok adjacent ranges coalesce\n");
	}

	/* Whichever order they arrive in. */
	{
		ChunkMap map(2048);
		map.addRange(1024, 2048);
		map.addRange(0, 1024);

		CHECK(map.getRangeCount() == 1);
		CHECK(ranges(map) == "0-2048");
		check(map);
		printf("  ok they coalesce backwards too\n");
	}

	/* Overlapping ranges merge rather than being counted twice. */
	{
		ChunkMap map(1000);
		map.addRange(0, 500);
		map.addRange(400, 800);

		CHECK(map.getRangeCount() == 1);
		CHECK(ranges(map) == "0-800");
		CHECK(map.getDoneBytes() == 800);
		check(map);
		printf("  ok overlapping ranges are not counted twice\n");
	}

	/* One range bridging several swallows them all. */
	{
		ChunkMap map(1000);
		map.addRange(0, 100);
		map.addRange(200, 300);
		map.addRange(400, 500);
		CHECK(map.getRangeCount() == 3);

		map.addRange(50, 450);
		CHECK(map.getRangeCount() == 1);
		CHECK(ranges(map) == "0-500");
		check(map);
		printf("  ok a range bridging several merges all of them\n");
	}

	/* Adding what is already known changes nothing. */
	{
		ChunkMap map(1000);
		map.addRange(100, 200);
		const std::string before = ranges(map);

		map.addRange(100, 200);
		map.addRange(120, 180);
		CHECK(ranges(map) == before);
		CHECK(map.getDoneBytes() == 100);
		check(map);
		printf("  ok adding a known range changes nothing\n");
	}

	/* Out of order insertions still come out sorted. */
	{
		ChunkMap map(1000);
		map.addRange(600, 700);
		map.addRange(100, 200);
		map.addRange(400, 500);

		CHECK(ranges(map) == "100-200,400-500,600-700");
		check(map);
		printf("  ok ranges are kept sorted: %s\n", ranges(map).c_str());
	}

	/* An empty or reversed range is not a range. */
	{
		ChunkMap map(1000);
		map.addRange(100, 100);
		map.addRange(300, 200);
		CHECK(map.getRangeCount() == 0);
		CHECK(map.getDoneBytes() == 0);
		printf("  ok an empty or reversed range is ignored\n");
	}

	/* Nothing may be recorded past the end of the file. */
	{
		ChunkMap map(1000);
		map.addRange(900, 5000);
		CHECK(ranges(map) == "900-1000");
		CHECK(map.getDoneBytes() == 100);
		check(map);
		printf("  ok a range past the end of the file is clamped\n");
	}

	/* Being told the size afterwards discards what no longer fits. */
	{
		ChunkMap map;
		map.addRange(0, 100);
		map.addRange(500, 900);

		map.setSize(600);
		CHECK(ranges(map) == "0-100,500-600");
		CHECK(map.getSize() == 600);
		check(map);

		map.setSize(200);
		CHECK(ranges(map) == "0-100");
		check(map);
		printf("  ok setting the size discards what is outside it\n");
	}

	/* Complete means one range covering all of it, and nothing less. */
	{
		ChunkMap map(1000);
		map.addRange(0, 999);
		CHECK(!map.isComplete());

		map.addRange(999, 1000);
		CHECK(map.isComplete());
		CHECK(map.getRangeCount() == 1);

		ChunkMap gap(1000);
		gap.addRange(0, 400);
		gap.addRange(500, 1000);
		CHECK(!gap.isComplete());
		CHECK(gap.getDoneBytes() == 900);
		printf("  ok a file with a hole in it is not complete\n");
	}

	/* How far the run holding an offset reaches. */
	{
		ChunkMap map(1000);
		map.addRange(100, 200);
		map.addRange(300, 400);

		CHECK(map.getRunEnd(100) == 200);
		CHECK(map.getRunEnd(150) == 200);
		CHECK(map.getRunEnd(199) == 200);

		/* Not downloaded, so it reaches nowhere. */
		CHECK(map.getRunEnd(200) == 200);
		CHECK(map.getRunEnd(0) == 0);
		CHECK(map.getRunEnd(350) == 400);
		printf("  ok getRunEnd reports the end of the run it is inside\n");
	}

	/* Failing the hash means forgetting the lot. */
	{
		ChunkMap map(1000);
		map.addRange(0, 1000);
		CHECK(map.isComplete());

		map.clear();
		CHECK(map.getRangeCount() == 0);
		CHECK(map.getDoneBytes() == 0);
		CHECK(!map.isComplete());
		CHECK(map.getSize() == 1000);
		printf("  ok clearing forgets the ranges but not the size\n");
	}

	/* The first free part of the file, no bigger than asked for. */
	{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		CHECK(map.reserve(1024, start, length));
		CHECK(start == 0);
		CHECK(length == 1024);
		CHECK(map.getReservationCount() == 1);

		/* And the next one starts where that ended. */
		CHECK(map.reserve(1024, start, length));
		CHECK(start == 1024);
		CHECK(length == 1024);
		CHECK(map.getReservationCount() == 2);
		printf("  ok reservations are handed out in order and do not overlap\n");
	}

	/* Downloaded parts are skipped, and a gap is not exceeded. */
	{
		ChunkMap map(10000);
		map.addRange(0, 1000);
		map.addRange(1500, 10000);

		uint64_t start = 0, length = 0;
		CHECK(map.reserve(4096, start, length));
		CHECK(start == 1000);
		CHECK(length == 500);

		/* That was the only hole. */
		CHECK(!map.hasFreeRange());
		CHECK(!map.reserve(4096, start, length));
		printf("  ok a reservation fills the hole exactly and no more\n");
	}

	/* Nothing to hand out. */
	{
		ChunkMap done(1000);
		done.addRange(0, 1000);
		uint64_t start = 0, length = 0;
		CHECK(!done.hasFreeRange());
		CHECK(!done.reserve(100, start, length));

		/* Neither without a size, nor for a chunk of nothing. */
		ChunkMap unknown;
		CHECK(!unknown.reserve(100, start, length));
		ChunkMap map(1000);
		CHECK(!map.reserve(0, start, length));
		printf("  ok nothing is reserved when there is nothing to reserve\n");
	}

	/* Asking whether there is anything left must not take it. */
	{
		ChunkMap map(1000);
		CHECK(map.hasFreeRange());
		CHECK(map.getReservationCount() == 0);

		CHECK(map.hasFreeRange());
		CHECK(map.getRangeCount() == 0);
		printf("  ok hasFreeRange does not reserve anything itself\n");
	}

	/* The reservation delivered. */
	{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		CHECK(map.reserve(1000, start, length));

		map.commit(start, length);
		CHECK(map.getReservationCount() == 0);
		CHECK(ranges(map) == "0-1000");
		CHECK(map.getDoneBytes() == 1000);
		check(map);
		printf("  ok committing a reservation promotes it to downloaded\n");
	}

	/*
	 * A peer is allowed to answer with less than it was asked for. What did
	 * arrive is kept, and the rest has to be free for someone else.
	 */
	{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		CHECK(map.reserve(4000, start, length));
		CHECK(length == 4000);

		map.commit(start, 1500);
		CHECK(ranges(map) == "0-1500");
		CHECK(map.getReservationCount() == 0);

		CHECK(map.reserve(4000, start, length));
		CHECK(start == 1500);
		printf("  ok a short commit hands the remainder back\n");
	}

	/* The reservation failed. */
	{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		CHECK(map.reserve(1000, start, length));
		map.release(start, length);

		CHECK(map.getReservationCount() == 0);
		CHECK(map.getRangeCount() == 0);

		/* And it is on offer again. */
		CHECK(map.reserve(1000, start, length));
		CHECK(start == 0);
		printf("  ok releasing a reservation keeps nothing and frees the range\n");
	}

	/* One release frees one reservation, not its neighbours. */
	{
		ChunkMap map(10000);

		uint64_t a = 0, b = 0, len = 0;
		CHECK(map.reserve(1000, a, len));
		CHECK(map.reserve(1000, b, len));
		CHECK(map.getReservationCount() == 2);

		map.release(b, len);
		CHECK(map.getReservationCount() == 1);

		uint64_t start = 0, length = 0;
		CHECK(map.reserve(1000, start, length));
		CHECK(start == b);
		printf("  ok releasing one reservation leaves the others alone\n");
	}

	/* Losing track of who has what. */
	{
		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		CHECK(map.reserve(1000, start, length));
		CHECK(map.reserve(1000, start, length));

		map.clearReservations();
		CHECK(map.getReservationCount() == 0);
		CHECK(map.reserve(1000, start, length));
		CHECK(start == 0);
		printf("  ok clearing the reservations offers the file again\n");
	}

	/* Reserving and committing the whole file, a chunk at a time. */
	{
		ChunkMap map(10000);

		int rounds = 0;
		uint64_t start = 0, length = 0;
		while (map.reserve(1024, start, length) && rounds++ < 100)
			map.commit(start, length);

		CHECK(map.isComplete());
		CHECK(map.getRangeCount() == 1);
		CHECK(map.getDoneBytes() == 10000);
		CHECK(map.getReservationCount() == 0);
		check(map);
		printf("  ok a file reserved and committed in chunks ends as one range\n");
	}

	/* A source that only holds part of the file can only be asked for that. */
	{
		ChunkMap theirs(10000);
		theirs.addRange(5000, 7000);

		ChunkMap map(10000);

		uint64_t start = 0, length = 0;
		CHECK(map.hasFreeRangeWithin(theirs));
		CHECK(map.getReservationCount() == 0);

		CHECK(map.reserveWithin(theirs, 4096, start, length));
		CHECK(start == 5000);
		CHECK(length == 2000);

		/* That was all they had. */
		CHECK(!map.hasFreeRangeWithin(theirs));
		CHECK(!map.reserveWithin(theirs, 4096, start, length));

		/* But the rest of the file is still there for someone else. */
		CHECK(map.hasFreeRange());
		CHECK(map.reserve(4096, start, length));
		CHECK(start == 0);
		CHECK(length == 4096);
		printf("  ok a partial source is only asked for what it has\n");
	}

	/* What we already have, inside what they have, is skipped. */
	{
		ChunkMap theirs(10000);
		theirs.addRange(0, 8000);

		ChunkMap map(10000);
		map.addRange(0, 3000);

		uint64_t start = 0, length = 0;
		CHECK(map.reserveWithin(theirs, 1000, start, length));
		CHECK(start == 3000);
		CHECK(length == 1000);

		/* And so is what is already in flight. */
		CHECK(map.reserveWithin(theirs, 1000, start, length));
		CHECK(start == 4000);
		printf("  ok a partial source is not asked for what we already have\n");
	}

	/* A source with nothing we need. */
	{
		ChunkMap theirs(10000);
		theirs.addRange(0, 1000);

		ChunkMap map(10000);
		map.addRange(0, 1000);

		uint64_t start = 0, length = 0;
		CHECK(!map.hasFreeRangeWithin(theirs));
		CHECK(!map.reserveWithin(theirs, 1000, start, length));

		ChunkMap nothing(10000);
		CHECK(!map.hasFreeRangeWithin(nothing));
		printf("  ok a source holding nothing we need is no use\n");
	}

	/* The queue file notation. */
	{
		ChunkMap map(1048576);
		map.addRange(0, 262144);
		map.addRange(524288, 1048576);

		CHECK(map.toString() == "0-262144,524288-1048576");

		ChunkMap back(1048576);
		CHECK(back.fromString(map.toString().c_str()));
		CHECK(back.toString() == map.toString());
		CHECK(back.getDoneBytes() == map.getDoneBytes());
		check(back);
		printf("  ok the queue notation survives a round trip\n");
	}

	/* Nothing downloaded is an empty string, both ways. */
	{
		ChunkMap map(1000);
		CHECK(map.toString().empty());
		CHECK(map.fromString(""));
		CHECK(map.getRangeCount() == 0);
		CHECK(map.fromString(0));
		printf("  ok an empty map encodes and decodes as nothing\n");
	}

	/* Reading replaces whatever was there. */
	{
		ChunkMap map(1000);
		map.addRange(0, 500);
		CHECK(map.fromString("600-700"));
		CHECK(map.toString() == "600-700");
		printf("  ok reading a map replaces the previous contents\n");
	}

	/* Overlapping input is coalesced as it is read. */
	{
		ChunkMap map(1000);
		CHECK(map.fromString("400-600,0-500"));
		CHECK(map.toString() == "0-600");
		check(map);
		printf("  ok overlapping input is coalesced on the way in\n");
	}

	/*
	 * Malformed input leaves the map empty rather than half parsed, so a queue
	 * file somebody edited cannot turn into a map claiming parts we do not have.
	 */
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
				printf("FAIL %s:%d: accepted \"%s\" as \"%s\"\n",
					__FILE__, __LINE__, bad[n], map.toString().c_str());
				failures++;
			}

			/* And nothing of it was kept. */
			CHECK(map.getRangeCount() == 0);
		}
		printf("  ok malformed queue notation is refused and keeps nothing\n");
	}

	/*
	 * strtoull() takes a sign and negates modulo 2^64, so a stray minus turns
	 * into a range reaching the end of the file - which is to say a partial
	 * download claiming to be complete.
	 */
	{
		ChunkMap map(100000);
		CHECK(!map.fromString("0--5"));
		CHECK(map.getRangeCount() == 0);
		CHECK(!map.isComplete());
		printf("  ok a negative bound does not become the whole file\n");
	}

	/* A number too big for the type is not a number we know. */
	{
		ChunkMap map(100000);
		CHECK(!map.fromString("0-99999999999999999999999999"));
		CHECK(map.getRangeCount() == 0);
		printf("  ok a bound that does not fit is refused\n");
	}

	/* The PSR notation counts whole blocks, not bytes. */
	{
		ChunkMap map(10000);
		map.addRange(0, 3000);

		/* Blocks 0 and 1 of 1000 are whole; the run ends before block 3. */
		CHECK(map.countBlocks(1000) == 3);
		CHECK(map.countBlockRuns(1000) == 1);
		CHECK(map.toIndexString(1000) == "0,3");

		map.addRange(5000, 6000);
		CHECK(map.countBlocks(1000) == 4);
		CHECK(map.countBlockRuns(1000) == 2);
		CHECK(map.toIndexString(1000) == "0,3,5,6");
		printf("  ok the block notation counts whole blocks: %s\n",
			map.toIndexString(1000).c_str());
	}

	/* Half a block is not a block. */
	{
		ChunkMap map(10000);
		map.addRange(0, 1500);

		CHECK(map.countBlocks(1000) == 1);
		CHECK(map.toIndexString(1000) == "0,1");
		printf("  ok a partly downloaded block does not count\n");
	}

	/* The last block is short when the file is not a whole number of them. */
	{
		ChunkMap map(2500);
		map.addRange(0, 2500);
		CHECK(map.isComplete());

		CHECK(map.countBlocks(1000) == 3);
		CHECK(map.toIndexString(1000) == "0,3");

		ChunkMap back(2500);
		CHECK(back.fromIndexString("0,3", 1000));
		CHECK(back.isComplete());
		CHECK(back.toString() == "0-2500");
		check(back);
		printf("  ok a short last block is a whole block of the file\n");
	}

	/* What a peer sends us in a PSR. */
	{
		ChunkMap map(10000);
		CHECK(map.fromIndexString("0,2,5,7", 1000));
		CHECK(map.toString() == "0-2000,5000-7000");
		CHECK(map.getDoneBytes() == 4000);
		check(map);

		CHECK(map.toIndexString(1000) == "0,2,5,7");
		printf("  ok the block notation survives a round trip\n");
	}

	/* Nothing at all is a valid thing for a peer to claim. */
	{
		ChunkMap map(10000);
		CHECK(map.fromIndexString("", 1000));
		CHECK(map.getRangeCount() == 0);
		CHECK(map.toIndexString(1000).empty());
		printf("  ok a peer holding nothing decodes as nothing\n");
	}

	/*
	 * And what a peer sends that is not a PSR at all. This one arrives straight
	 * off the network, so every one of these is somebody else's input.
	 */
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
				printf("FAIL %s:%d: accepted \"%s\" as \"%s\"\n",
					__FILE__, __LINE__, bad[n], map.toString().c_str());
				failures++;
			}
			CHECK(map.getRangeCount() == 0);
		}
		printf("  ok malformed block notation is refused and keeps nothing\n");
	}

	/* A block index beyond the end of the file is not ours to believe. */
	{
		ChunkMap map(10000);
		CHECK(!map.fromIndexString("20,30", 1000));
		CHECK(map.getRangeCount() == 0);
		printf("  ok blocks past the end of the file are refused\n");
	}

	/* A run overhanging the end is clamped to it rather than refused. */
	{
		ChunkMap map(2500);
		CHECK(map.fromIndexString("0,4", 1000));
		CHECK(map.toString() == "0-2500");
		CHECK(map.isComplete());
		check(map);
		printf("  ok a run overhanging the end is clamped to the file\n");
	}

	/* Without a block size there is nothing to count. */
	{
		ChunkMap map(10000);
		map.addRange(0, 10000);
		CHECK(map.countBlocks(0) == 0);
		CHECK(map.countBlockRuns(0) == 0);
		CHECK(map.toIndexString(0).empty());
		CHECK(!map.fromIndexString("0,2", 0));
		printf("  ok a block size of zero counts nothing\n");
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
