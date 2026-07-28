/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CHUNKMAP_H
#define HAVE_QUICKDC_CHUNKMAP_H

#include "quickdc.h"
#include <string>
#include <vector>

namespace QuickDC {

/**
 * Keeps track of which parts of a file have been downloaded, and which
 * parts are currently being downloaded by someone.
 *
 * All ranges are half open; [start, end), which means a range of 0-1024
 * covers the first 1024 bytes, and the next range starts at 1024.
 * This matches the on-disk notation described in downloadqueue.h:
 *
 *     chunks: 0-262144,524288-1048576
 *
 * The completed ranges are kept sorted, non-overlapping and coalesced, so
 * a complete file is always exactly one range covering [0, size).
 *
 * Reservations exist only in memory and are deliberately never written to
 * disk. Whatever was in flight when we died is simply downloaded again;
 * that is cheaper than being wrong about what actually reached the disk.
 */
class ChunkMap {
	public:
		ChunkMap(uint64_t size = 0);

		/**
		 * Set the total size of the file. Ranges beyond the new size
		 * are discarded.
		 */
		void setSize(uint64_t size);
		uint64_t getSize() const { return size; }

	public:
		/**
		 * Mark [start, end) as downloaded. Overlapping and adjacent
		 * ranges are merged, so this is safe to call repeatedly with
		 * ranges that already are (partially) known.
		 */
		void addRange(uint64_t start, uint64_t end);

		/**
		 * @return true if the whole file is downloaded.
		 * Note: a file of unknown size (0) is never complete.
		 */
		bool isComplete() const;

		/**
		 * @return the number of bytes downloaded so far.
		 */
		uint64_t getDoneBytes() const;

		size_t getRangeCount() const { return done.size(); }

		/**
		 * Read out completed range number 'idx'.
		 * @return false if idx is out of range.
		 */
		bool getRange(size_t idx, uint64_t& start, uint64_t& end) const;

		/**
		 * Remove all knowledge of what has been downloaded.
		 * Used when a completed file fails hash verification.
		 */
		void clear();

	public:
		/**
		 * Find the first part of the file that is neither downloaded nor
		 * already reserved by someone else, and reserve it.
		 *
		 * @param preferred the desired chunk size, the reservation will
		 *                  never be larger than this, but may be smaller
		 *                  if the gap is smaller.
		 * @return false if there is nothing left to download.
		 */
		bool reserve(uint64_t preferred, uint64_t& start, uint64_t& length);

		/**
		 * @return true if reserve() would hand something out.
		 */
		bool hasFreeRange() const;

		/** As reserve(), but only from parts that 'allowed' also holds. */
		bool reserveWithin(const ChunkMap& allowed, uint64_t preferred,
			uint64_t& start, uint64_t& length);

		/**
		 * The reservation delivered; promote 'length' bytes of it to a
		 * downloaded range and drop the reservation.
		 *
		 * 'length' may be shorter than what was reserved, in which case the
		 * unfulfilled remainder becomes available again.
		 */
		void commit(uint64_t start, uint64_t length);

		/**
		 * The reservation failed; drop it and keep nothing.
		 *
		 * The reservation is identified by 'start' alone; 'length' is accepted
		 * for symmetry with commit() and reserve() but is not matched on.
		 */
		void release(uint64_t start, uint64_t length);

		/**
		 * Drop every reservation. Used when the queue is loaded, and
		 * whenever we can no longer account for who is downloading what.
		 */
		void clearReservations();

		size_t getReservationCount() const { return reserved.size(); }

	public:
		/**
		 * Encode the completed ranges as "0-262144,524288-1048576".
		 * An empty map encodes as an empty string.
		 */
		std::string toString() const;

		/**
		 * Decode the notation written by toString(). The existing
		 * contents are replaced.
		 *
		 * @return false if the input is malformed, in which case the map
		 *         is left empty rather than half parsed.
		 */
		bool fromString(const char* str);

		/**
		 * Encode the completed ranges as block index pairs, "0,1,4,5" for
		 * blocks 0 and 4. This is the PI field of an ADC PSR message.
		 */
		std::string toIndexString(uint64_t blockSize) const;

		/**
		 * Decode the notation written by toIndexString(). The existing
		 * contents are replaced.
		 *
		 * @return false if the input is malformed, in which case the map
		 *         is left empty rather than half parsed.
		 */
		bool fromIndexString(const char* str, uint64_t blockSize);

		/** How many whole blocks are complete; the PC field of a PSR. */
		size_t countBlocks(uint64_t blockSize) const;

	protected:
		struct Range {
			uint64_t start;
			uint64_t end;
		};

		/**
		 * @return true if [start, end) overlaps any reservation.
		 */
		bool isReserved(uint64_t start, uint64_t end) const;

		/** @return true if all of block 'index' is downloaded. */
		bool hasBlock(uint64_t blockSize, uint64_t index) const;

	protected:
		std::vector<Range> done;      // sorted, non-overlapping, coalesced
		std::vector<Range> reserved;  // in flight, never persisted
		uint64_t size;
};

}

#endif // HAVE_QUICKDC_CHUNKMAP_H
