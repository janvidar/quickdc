/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_FILESOURCE_H
#define HAVE_QUICKDC_FILESOURCE_H

#include "quickdc.h"
#include <string.h>
#include <stdlib.h>
#include <samurai/timestamp.h>
#include "network/chunkmap.h"
#include <vector>

/** A source is forgotten after this many consecutive failures. */
#define QUICKDC_SOURCE_MAX_RETRIES 50

/** How long to leave a source alone after it failed us. */
#define QUICKDC_SOURCE_RETRY_INTERVAL 60

/** A source is forgotten when we have not seen it for this long (30 days). */
#define QUICKDC_SOURCE_MAX_AGE (30*24*60*60)

namespace QuickDC {

/**
 * A file source is basically a source for downloading a specific file.
 * This does not contain any information about the file itself.
 * For obvious reasons this scheme works best with the ADC protocol, but
 * the NMDC protocol with extensions support alot of the needed stuff here,
 * except the needed clientID and thus the base32 encoded nick is used instead.
 *
 * This is basically a URL notation.
 *
 * Examples:
 * dchub://host:port/clientID/file (1)
 *   adc://host:port/clientID/TTH:xxxxxxxxxxx
 *
 * A source for a directory is encoded as:
 * {protocol}://host:port/{clientID}/dir/
 *
 * A source for the root directory (all shares) is encoded as follows:
 * {protocol}://host:port/{clientID}/ (2)
 *
 * A source for the file list (index) is encoded as follows:
 * {protocol}://host:port/{clientID} (3)
 *
 * 1) NOTE: clientID is basically a nick for 'dchub' protocol,
 *          This will have to be escaped so that it does not contain
 *          any characters that will confuse decoding the URL.
 *          that means '/'.
 * 2) NOTE: Must have trailing slash. See 4.
 * 3) Without the trailing slash this means download the file listing,
 *    however with the trailing slash this means download the root
 *    directory.
 *
 * The distinction in 3) is carried by 'filename': a null filename means the
 * file list, while an empty but non-null filename means the root directory.
 */
class FileSource {
	public:

		/**
		 * A filesource might be a on the form: adc://{server}:{port}/{cid}
		 * Use isValid() to check whether the URL could be decoded at all.
		 */
		FileSource(const char* url);

		/**
		 * Create a file source. This is usually created from a search reply,
		 * but might also be created from parsing a file list.
		 *
		 * @param hub      A hub URL, example: adc://host:port
		 * @param userID   A user ID, such as a CID (ADC) or nick (NMDC)
		 * @param filename Filename, might be 0 indicating the TTH root should be used.
		 *
		 * The filename might be set, if not set use TTH-sum based on the QueueFile.
		 * If the filename is "index" without a preceding "/", the file source
		 * interpreted as downloading the file list.
		 */
		FileSource(const char* hub, const char* userID, const char* filename = 0);

		virtual ~FileSource();

	public:

		/**
		 * @return the filename in utf-8, however might be 0 if no filename is known.
		 */
		inline const char* getName() const { return filename; }

		/**
		 * @return the Client/User ID where the file source is detected.
		 */
		inline const char* getClientID() const { return cid; }

		/**
		 * @return the URL to the hub where the user is connected to.
		 */
		inline const char* getHubUrl() const { return hub; }

		/**
		 * @return false if this source could not be decoded, and thus can
		 *         neither be used nor stored.
		 */
		inline bool isValid() const { return (hub && cid); }

		/**
		 * @return true if this source refers to a file list rather than to
		 *         a file or a directory.
		 */
		inline bool isFilelist() const { return (filename == 0); }

		/**
		 * Encode this source back into the URL notation described above.
		 * This is what is written to the download queue on disk.
		 *
		 * @return a newly allocated string which the caller must free(),
		 *         or 0 if this source is not valid.
		 */
		char* toURL() const;

	public:

		/**
		 * The source was seen again; in a search reply, in a file list, or
		 * because we transferred something from it. Clears the failure count.
		 */
		void touch();

		/**
		 * We failed to get anything useful out of this source.
		 */
		void onFailure();

		/** We are about to ask this source for something. */
		void onAttempt();

		/**
		 * Which parts of the file this source has, as a PSR "PI" field. Kept
		 * as it arrived, since decoding needs a block size we may not know yet.
		 */
		void setAvailability(const char* indexes);

		inline bool isPartial() const { return (availability != 0); }

		bool getAvailability(uint64_t blockSize, uint64_t fileSize, ChunkMap& out) const;

		/**
		 * @return false if this source failed recently and should be left
		 *         alone for now. A busy peer must not be hammered.
		 */
		bool isReadyToRetry() const;

		inline size_t getRetryCount() const { return retryCount; }
		inline const Samurai::TimeStamp& getLastSeen() const { return lastSeen; }

		/**
		 * How fast this source has actually been handing chunks over, in bytes per
		 * second, or zero until one has arrived.
		 *
		 * Measured from asking to arriving, so the round trip that each chunk costs
		 * is counted in it. That is the point of the figure: it decides how much to
		 * ask for next time, and asking for too little is precisely what makes the
		 * overhead worth counting.
		 *
		 * Not persisted. A source's throughput says as much about the network in
		 * between as about the peer, and neither is worth assuming a week later.
		 */
		uint64_t getThroughput() const;

		/**
		 * A chunk arrived: this many bytes, this long after it was asked for.
		 *
		 * Ignored if either is zero, which is what a request that failed or was
		 * answered with nothing looks like.
		 */
		void onChunkDone(uint64_t bytes, double seconds);

		/**
		 * @return true if this source has failed too many times, or has not
		 *         been seen for too long, and should be forgotten.
		 */
		bool isExpired() const;

		/**
		 * @return true if this source refers to the given user on the given hub.
		 */
		bool matches(const char* hub, const char* cid) const;

	public: /* used when loading the queue from disk */

		void setRetryCount(size_t count);
		void setLastSeen(const Samurai::TimeStamp& seen);

	protected:
		char* hub;            // (adcs?|dchub)://(host(:port)?)
		char* cid;            // clientID (CID on ADC, nick on NMDC).
		char* filename;       // filename, full path, or NULL (tth is used instead).
		size_t retryCount;    // expire this after ~50 tries
		Samurai::TimeStamp lastSeen; // expire this after ~30 days.
		Samurai::TimeStamp lastAttempt; // not persisted; a restart may retry at once.
		char* availability;   // PSR "PI" notation, or 0 if they have it all
		double throughput;    // bytes per second, smoothed; 0 until a chunk arrives
};

}

#endif // HAVE_QUICKDC_FILESOURCE_H
