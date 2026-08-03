/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_LEAFHASHES_H
#define HAVE_QUICKDC_LEAFHASHES_H

#include "quickdc.h"
#include <stdlib.h>
#include <vector>

namespace Samurai {
	namespace IO {
		class File;
	}
}

namespace QuickDC {

/**
 * The leaf hashes of a file's tiger tree, as delivered by a "tthl" transfer.
 *
 * These are what make a single chunk verifiable on its own. Without them a
 * download can only be checked once every byte is in, so one bad source costs
 * the whole file, and nothing can be re-shared before it is finished.
 */
class LeafHashes {
	public:
		LeafHashes();
		virtual ~LeafHashes();

		/**
		 * Take on leaf data received from a peer.
		 *
		 * The leaves are hashed back up into a root and compared against the
		 * one we already trust, so a peer cannot talk us into verifying its
		 * corruption. Anything that does not add up is rejected.
		 *
		 * @param tth  the expected root, base32, as it appears in the queue.
		 * @return false if the data is unusable, in which case nothing is kept.
		 */
		bool load(const char* tth, uint64_t fileSize, const char* data, size_t length);

		/** True once load() has succeeded. */
		inline bool isLoaded() const { return !leaves.empty(); }

		/** How much of the file one leaf covers. */
		inline uint64_t getLeafSize() const { return leafSize; }
		inline size_t getLeafCount() const { return leafCount; }

		/**
		 * Check a range of a file against the leaves covering it.
		 *
		 * @param offset must be on a leaf boundary.
		 * @param length must be a whole number of leaves, or reach the end.
		 * @return false if it does not match, or cannot be read.
		 */
		bool verify(Samurai::IO::File& file, uint64_t offset, uint64_t length) const;

		/**
		 * Where the leaves are kept for a given download.
		 * @return a newly allocated string which the caller must free(), or 0.
		 */
		static char* makeFilename(const char* tempFilename);

		bool save(const char* filename) const;
		bool load(const char* filename, const char* tth, uint64_t fileSize);

	protected:
		/* The leaf hashes back to back, hashSize bytes each; not a string. */
		std::vector<char> leaves;
		size_t leafCount;
		size_t hashSize;
		uint64_t leafSize;
		uint64_t fileSize;
};

}

#endif // HAVE_QUICKDC_LEAFHASHES_H
