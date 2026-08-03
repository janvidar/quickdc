/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/chunkmap.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

/*
 * Reads one plain decimal number and leaves 'p' on whatever follows it.
 *
 * strtoull() accepts a sign and negates modulo 2^64, and answers ULLONG_MAX for
 * anything too big, so "0--5" and a hundred digits both decode as a range
 * reaching the end of the file - which is to say a partial download claiming to
 * be complete. The same digits arrive from other peers in a PSR.
 */
static bool parseNumber(const char*& p, uint64_t& out)
{
	if (!isdigit(static_cast<unsigned char>(*p))) return false;

	char* endp = 0;
	errno = 0;
	out = static_cast<uint64_t>(strtoull(p, &endp, 10));
	if (errno == ERANGE) return false;

	p = endp;
	return true;
}

QuickDC::ChunkMap::ChunkMap(uint64_t size_) : size(size_)
{

}

void QuickDC::ChunkMap::setSize(uint64_t size_)
{
	size = size_;
	if (!size)
		return;

	/* Discard anything that no longer fits inside the file. */
	std::vector<Range> kept;
	for (size_t n = 0; n < done.size(); n++)
	{
		if (done[n].start >= size) continue;
		Range r = done[n];
		if (r.end > size) r.end = size;
		kept.push_back(r);
	}
	done = kept;
}

void QuickDC::ChunkMap::addRange(uint64_t start, uint64_t end)
{
	if (size && end > size) end = size;
	if (end <= start) return;

	Range r;
	r.start = start;
	r.end = end;

	/*
	 * Absorb every existing range that overlaps or merely touches the new
	 * one, and keep the rest. Comparing with < and > rather than <= and >=
	 * is what makes adjacent ranges coalesce: 0-1024 plus 1024-2048 has to
	 * become a single 0-2048.
	 */
	std::vector<Range> merged;
	for (size_t n = 0; n < done.size(); n++)
	{
		if (done[n].end < r.start || done[n].start > r.end)
		{
			merged.push_back(done[n]);
		}
		else
		{
			if (done[n].start < r.start) r.start = done[n].start;
			if (done[n].end > r.end) r.end = done[n].end;
		}
	}

	size_t idx = 0;
	while (idx < merged.size() && merged[idx].start < r.start) idx++;
	merged.insert(merged.begin() + idx, r);

	done = merged;
}

bool QuickDC::ChunkMap::isComplete() const
{
	if (!size) return false;
	if (done.size() != 1) return false;
	return (done[0].start == 0 && done[0].end == size);
}

uint64_t QuickDC::ChunkMap::getDoneBytes() const
{
	uint64_t total = 0;
	for (size_t n = 0; n < done.size(); n++)
		total += (done[n].end - done[n].start);
	return total;
}

bool QuickDC::ChunkMap::getRange(size_t idx, uint64_t& start, uint64_t& end) const
{
	if (idx >= done.size()) return false;
	start = done[idx].start;
	end = done[idx].end;
	return true;
}

uint64_t QuickDC::ChunkMap::getRunEnd(uint64_t start) const
{
	for (size_t n = 0; n < done.size(); n++)
		if (start >= done[n].start && start < done[n].end) return done[n].end;

	return start;
}

void QuickDC::ChunkMap::clear()
{
	done.clear();
}

bool QuickDC::ChunkMap::isReserved(uint64_t start, uint64_t end) const
{
	for (size_t n = 0; n < reserved.size(); n++)
	{
		if (reserved[n].start < end && start < reserved[n].end)
			return true;
	}
	return false;
}

bool QuickDC::ChunkMap::hasFreeRange() const
{
	uint64_t start = 0;
	uint64_t length = 0;

	ChunkMap copy(*this);
	return copy.reserve(1, start, length);
}

bool QuickDC::ChunkMap::reserve(uint64_t preferred, uint64_t& start, uint64_t& length)
{
	if (!size || !preferred) return false;

	/*
	 * Walk forward past everything that is already downloaded or reserved.
	 * Neither vector is sorted with respect to the other, so keep going
	 * until a full pass moves us nowhere.
	 */
	uint64_t pos = 0;
	for (;;)
	{
		bool moved = false;

		for (size_t n = 0; n < done.size(); n++)
		{
			if (pos >= done[n].start && pos < done[n].end)
			{
				pos = done[n].end;
				moved = true;
			}
		}
		for (size_t n = 0; n < reserved.size(); n++)
		{
			if (pos >= reserved[n].start && pos < reserved[n].end)
			{
				pos = reserved[n].end;
				moved = true;
			}
		}

		if (!moved || pos >= size) break;
	}

	if (pos >= size) return false;

	/* The gap ends at whatever comes next, or at the end of the file. */
	uint64_t end = size;
	for (size_t n = 0; n < done.size(); n++)
		if (done[n].start > pos && done[n].start < end) end = done[n].start;
	for (size_t n = 0; n < reserved.size(); n++)
		if (reserved[n].start > pos && reserved[n].start < end) end = reserved[n].start;

	if (end - pos > preferred) end = pos + preferred;

	start = pos;
	length = end - pos;

	Range r;
	r.start = start;
	r.end = end;
	reserved.push_back(r);

	return true;
}

bool QuickDC::ChunkMap::hasFreeRangeWithin(const ChunkMap& allowed) const
{
	uint64_t start = 0;
	uint64_t length = 0;

	ChunkMap copy(*this);
	return copy.reserveWithin(allowed, 1, start, length);
}

bool QuickDC::ChunkMap::reserveWithin(const ChunkMap& allowed, uint64_t preferred,
	uint64_t& start, uint64_t& length)
{
	if (!size || !preferred) return false;

	for (size_t n = 0; n < allowed.done.size(); n++)
	{
		uint64_t from = allowed.done[n].start;
		uint64_t to = allowed.done[n].end;
		if (to > size) to = size;

		while (from < to)
		{
			bool blocked = false;
			for (size_t i = 0; i < done.size(); i++)
			{
				if (from >= done[i].start && from < done[i].end) { from = done[i].end; blocked = true; }
			}
			for (size_t i = 0; i < reserved.size(); i++)
			{
				if (from >= reserved[i].start && from < reserved[i].end) { from = reserved[i].end; blocked = true; }
			}
			if (blocked) continue;
			if (from >= to) break;

			uint64_t end = to;
			for (size_t i = 0; i < done.size(); i++)
				if (done[i].start > from && done[i].start < end) end = done[i].start;
			for (size_t i = 0; i < reserved.size(); i++)
				if (reserved[i].start > from && reserved[i].start < end) end = reserved[i].start;

			if (end - from > preferred) end = from + preferred;
			if (end <= from) break;

			start = from;
			length = end - from;

			Range r;
			r.start = start;
			r.end = end;
			reserved.push_back(r);
			return true;
		}
	}
	return false;
}

void QuickDC::ChunkMap::commit(uint64_t start, uint64_t length)
{
	release(start, length);
	addRange(start, start + length);
}

void QuickDC::ChunkMap::release(uint64_t start, uint64_t length)
{
	/*
	 * Matched on the start offset alone, deliberately. A peer is allowed to
	 * answer with less than it was asked for, so the committed length is often
	 * shorter than the reservation; dropping the whole reservation hands the
	 * remainder back to whoever asks next. reserve() never produces two
	 * reservations with the same start, so this cannot free someone else's.
	 */
	(void) length;

	for (std::vector<Range>::iterator it = reserved.begin(); it != reserved.end(); it++)
	{
		if ((*it).start == start)
		{
			reserved.erase(it);
			return;
		}
	}
}

void QuickDC::ChunkMap::clearReservations()
{
	reserved.clear();
}

std::string QuickDC::ChunkMap::toString() const
{
	std::string out;
	for (size_t n = 0; n < done.size(); n++)
	{
		if (n) out += ",";
		out += std::to_string(done[n].start);
		out += "-";
		out += std::to_string(done[n].end);
	}
	return out;
}

/*
 * A block is only ours if all of it is; the last one is short when the file
 * size is not a whole number of blocks.
 */
bool QuickDC::ChunkMap::hasBlock(uint64_t blockSize, uint64_t index) const
{
	uint64_t start = index * blockSize;
	uint64_t end = start + blockSize;
	if (size && end > size) end = size;
	if (end <= start) return false;

	for (size_t n = 0; n < done.size(); n++)
		if (done[n].start <= start && done[n].end >= end) return true;

	return false;
}

size_t QuickDC::ChunkMap::countBlocks(uint64_t blockSize) const
{
	if (!blockSize || !size) return 0;

	size_t blocks = static_cast<size_t>((size + blockSize - 1) / blockSize);
	size_t have = 0;
	for (size_t n = 0; n < blocks; n++)
		if (hasBlock(blockSize, n)) have++;

	return have;
}

size_t QuickDC::ChunkMap::countBlockRuns(uint64_t blockSize) const
{
	if (!blockSize || !size) return 0;

	size_t blocks = static_cast<size_t>((size + blockSize - 1) / blockSize);
	size_t runs = 0;

	for (size_t n = 0; n < blocks; )
	{
		if (!hasBlock(blockSize, n)) { n++; continue; }
		runs++;
		while (n < blocks && hasBlock(blockSize, n)) n++;
	}
	return runs;
}

std::string QuickDC::ChunkMap::toIndexString(uint64_t blockSize) const
{
	std::string out;
	if (!blockSize || !size) return out;

	size_t blocks = static_cast<size_t>((size + blockSize - 1) / blockSize);

	for (size_t n = 0; n < blocks; )
	{
		if (!hasBlock(blockSize, n)) { n++; continue; }

		size_t first = n;
		while (n < blocks && hasBlock(blockSize, n)) n++;

		if (out.size()) out += ",";
		out += std::to_string(static_cast<uint64_t>(first));
		out += ",";
		out += std::to_string(static_cast<uint64_t>(n));
	}
	return out;
}

bool QuickDC::ChunkMap::fromIndexString(const char* str, uint64_t blockSize)
{
	done.clear();
	if (!blockSize) return false;
	if (!str || !*str) return true;

	const char* p = str;
	while (*p)
	{
		uint64_t first = 0;
		if (!parseNumber(p, first) || *p != ',') { done.clear(); return false; }
		p++;

		uint64_t last = 0;
		if (!parseNumber(p, last) || last <= first) { done.clear(); return false; }

		/* Block indices are somebody else's arithmetic, so do not wrap on it. */
		if (last > UINT64_MAX / blockSize) { done.clear(); return false; }

		uint64_t start = first * blockSize;
		uint64_t end = last * blockSize;
		if (size && end > size) end = size;
		if (size && start >= size) { done.clear(); return false; }

		addRange(start, end);

		if (*p == ',')
		{
			p++;
			if (!*p) { done.clear(); return false; }
		}
		else if (*p) { done.clear(); return false; }
	}
	return true;
}

bool QuickDC::ChunkMap::fromString(const char* str)
{
	done.clear();
	if (!str || !*str) return true;

	const char* p = str;
	while (*p)
	{
		uint64_t start = 0;
		if (!parseNumber(p, start) || *p != '-') { done.clear(); return false; }
		p++;

		uint64_t end = 0;
		if (!parseNumber(p, end) || end <= start) { done.clear(); return false; }

		addRange(start, end);

		if (*p == ',')
		{
			p++;
			if (!*p) { done.clear(); return false; }
		}
		else if (*p)
		{
			done.clear();
			return false;
		}
	}

	return true;
}
