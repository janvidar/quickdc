/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DOWNLOADQUEUE_H
#define HAVE_QUICKDC_DOWNLOADQUEUE_H

#include "quickdc.h"
#include "api/subscription.h"
#include "network/chunkmap.h"
#include <string.h>
#include <stdlib.h>
#include <samurai/timestamp.h>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

/** Where the queue is kept between runs. */
#define QUICKDC_QUEUE_FILE     "~/.quickdc/queue.xml"
#define QUICKDC_QUEUE_FILE_NEW "~/.quickdc/queue.xml.new"

/** Downloaded file lists end up here. */
#define QUICKDC_LIST_DIR       "~/.quickdc/lists"

/** Default destination for downloads, overridable in [Downloads]/Directory. */

/** What a source we know nothing about yet is asked for. */
#define QUICKDC_CHUNK_SIZE (1024*1024)

/**
 * How much of a transfer one chunk should be worth, in seconds.
 *
 * A chunk costs a round trip: it is asked for only once the one before it has
 * arrived and been reported, so the connection sits idle in between. At a couple
 * of seconds a chunk that costs a few milliseconds, which is in the noise.
 */
#define QUICKDC_CHUNK_TARGET_SECONDS 2

/**
 * No chunk grows past this, however fast the source.
 *
 * A chunk that fails is released whole and fetched again from the start, so the
 * ceiling is what bounds the work a dropped connection can throw away.
 */
#define QUICKDC_CHUNK_SIZE_MAX (16*1024*1024)

/** A partially downloaded file carries this suffix until it is verified. */
#define QUICKDC_PART_SUFFIX ".part"

/** A file list request is abandoned if it has not completed within this long. */
#define QUICKDC_FILELIST_TIMEOUT 300

/** How long to wait before asking again for a file list we failed to request. */
#define QUICKDC_FILELIST_RETRY_INTERVAL 10

/** A file list request is given up on after this many failed attempts. */
#define QUICKDC_FILELIST_MAX_RETRIES 5

/** A peer that never came back with our data is forgotten after this long. */
#define QUICKDC_REQUEST_TIMEOUT 120

/** The queue is flushed to disk at most this often while running. */
#define QUICKDC_QUEUE_SAVE_INTERVAL 5

/** How long between automatic searches for more sources, one file at a time. */
#define QUICKDC_SOURCE_SEARCH_INTERVAL 120

/** Files with fewer sources than this are searched for. */
#define QUICKDC_SOURCE_SEARCH_MIN_SOURCES 5

/**
 * How many searches may be asked for on the spot, and how quickly one is
 * credited back.
 *
 * QueueManager::requestSourceSearch() is driven by whoever is reading rather
 * than by the clock, and a hub disconnects a client that floods it with
 * searches. A burst is allowed because opening a thread of replies asks for all
 * of their bodies at once and a reader is waiting for every one of them; the
 * credit is what stops a caller in a loop from becoming a flood, since once the
 * burst is spent the rate settles at one search per interval.
 */
#define QUICKDC_SOURCE_SEARCH_BURST 5
#define QUICKDC_SOURCE_SEARCH_BURST_REFILL 10

/** A TTH root in base32 is always this long. */
#define QUICKDC_TTH_LENGTH 39

/**
 * The longest identifier we will name a downloaded file list after.
 *
 * An ADC CID is 39 base32 characters and User::setNick() keeps at most 63
 * bytes of a nickname, so nothing legitimate comes anywhere near this.
 */
#define QUICKDC_IDENTIFIER_MAX 64

/**
 * The largest compressed file list we accept, and the largest it may unpack to.
 *
 * Both are needed: the first bounds what one download can make us read, and the
 * second is what stops a bzip2 bomb, which reaches about a million to one on
 * input crafted for it. A share of a quarter of a million files makes a list of
 * some tens of megabytes, so neither of these is within sight of a real one.
 */
#define QUICKDC_FILELIST_MAX_PACKED (16*1024*1024)
#define QUICKDC_FILELIST_MAX_PLAIN  (256*1024*1024)

/**
 * How deeply a peer's file list may nest before we stop believing it.
 *
 * The walk over the decoded tree is recursive, and nothing in the format stops
 * a peer nesting a directory inside itself until our stack runs out.
 */
#define QUICKDC_FILELIST_MAX_DEPTH 128

/**
 * How many elements a peer's file list may contain.
 *
 * Every one becomes a directory or a file held in memory, so this bounds what a
 * list costs to keep rather than what it costs to read - the size cap above
 * already bounds the reading. The smallest entry a list can carry is some seventy
 * bytes, so a list at that cap cannot hold many more of them than this; it is
 * here so that the memory has a stated bound rather than one that falls out of
 * two other numbers.
 */
#define QUICKDC_FILELIST_MAX_ENTRIES (4*1000*1000)

/**
 * Bounds on our own queue file.
 *
 * ~/.quickdc/queue.xml is written by this client and read back on the next start,
 * so these are what it writes rather than what a peer might send. A file past
 * them is one that grew in a way nothing here explains - a corruption, or an
 * edit - and is better refused than half loaded.
 */
#define QUICKDC_QUEUE_MAX_SIZE     (64*1024*1024)
#define QUICKDC_QUEUE_MAX_ELEMENTS (1000*1000)

namespace Samurai {
	namespace IO {
		class Buffer;
		class File;
	}
}

namespace Share {
	class RemoteDir;
}

namespace QuickDC {

class FileSource;
class LeafHashes;
class QueueManager;
class Core;
class QueueVerifier;
class QueueXMLReader;

/**
 * A file we want to download, and everywhere we know to get it from.
 *
 * The queue is stored as XML in ~/.quickdc/queue.xml:
 *
 * <?xml version="1.1" encoding="utf-8" standalone="yes"?>
 * <DownloadQueue version="1" generator="..." updated="1138411637">
 *     <File name="/home/user/downloads/file.bin" size="2097152"
 *           tth="MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A"
 *           added="1138411637" priority="normal">
 *         <Source url="dchub://myhub:411/{nick in base32}/file.bin" lastseen="1138411637" retries="0" />
 *         <Source url="adc://adchub:7817/ACSTOWQXZASA/TTH:MWXFLX5NO4T54FMVTGZ6UXER2QKN6J2IGGL3W7A" ... />
 *         <Chunk start="0" end="262144" />
 *         <Chunk start="524288" end="1048576" />
 *     </File>
 * </DownloadQueue>
 *
 * This means; we have a file locally called file.bin, which was queued
 * Saturday January 28, 2006, encoded in seconds since the UNIX epoch.
 * The file size is exactly 2MB, and we have a root hash of it.
 * We have 2 different sources for the file.
 * Currently the first 256KBytes are downloaded, followed by a 256KBytes gap,
 * the next 512Kbytes are completed, and the last 1024 kbytes are not.
 *
 * Note that not every queued file is written to disk; see isPersistent().
 */
class QueueFile {
	public:
		enum Priority {
			PrioPaused,
			PrioLow,
			PrioNormal,
			PrioHigh
		};

		/**
		 * @param filename the local file we are downloading to.
		 * @param tth      the TTH root in base32, or 0 if unknown.
		 * @param size     the size of the file, or 0 if unknown.
		 */
		QueueFile(const char* filename, const char* tth, uint64_t size = 0);
		virtual ~QueueFile();

		QueueFile(const QueueFile&) = delete;
		QueueFile& operator=(const QueueFile&) = delete;

		static const char* convertPriority(enum Priority);
		static enum Priority convertPriority(const char*);

	public:
		/** The file we are downloading to, once it is finished. */
		inline const char* getLocalFilename() const { return filename.c_str(); }

		/** The file we are downloading into while incomplete. */
		inline const char* getTempFilename() const { return tempname.c_str(); }

		/** The TTH root in base32, or 0 if we do not know it. */
		inline const char* getTTH() const { return tth.empty() ? 0 : tth.c_str(); }

		inline uint64_t getSize() const { return size; }
		void setSize(uint64_t size);

		inline const Samurai::TimeStamp& getTimeAdded() const { return added; }

		inline enum Priority getPriority() const { return priority; }
		inline void setPriority(enum Priority prio) { priority = prio; }

	public:
		/**
		 * Add a source. Ownership is taken over.
		 * A source we already know about is deleted instead of added, so it
		 * is safe to hand over every source we come across.
		 */
		void addSource(FileSource* source);

		/** Remove and delete a source. */
		void removeSource(FileSource* source);

		/** Forget every source that has failed too often or gone stale. */
		void expireSources();

		size_t countSources() const;
		size_t countActiveSources() const;

		FileSource* getSource(size_t n) const;
		FileSource* findSource(const char* hub, const char* cid) const;

		/**
		 * @return a source we are not currently downloading from and which
		 *         has not expired, or 0 if there is none.
		 */
		FileSource* getIdleSource() const;

		void setSourceActive(FileSource* source, bool active);
		bool isSourceActive(FileSource* source) const;

		/**
		 * Ask only this hub about this file, rather than every hub we are on.
		 *
		 * For a file whose hash is only one hub's business because that is where it
		 * was learned. A bulletin board post is the case: its hash is on that hub's
		 * board, where every subscriber can read it, and asking elsewhere tells
		 * another operator which boards this client reads - a disclosure the download
		 * does not need. It also asks people who cannot answer, since a post is
		 * offered by hash alone and only by the hub's own subscribers.
		 *
		 * Confining a file is a decision about where it may be looked for and not
		 * about where it may be fetched from: a source named by any other route is
		 * still a source.
		 *
		 * @param hubUrl canonical, as Hub::getUrl() returns it; 0 or empty lifts it.
		 */
		void searchOnlyOn(const char* hubUrl);

		/** The one hub to ask, or 0 where every hub may be asked. */
		const char* searchesOnlyOn() const;

		/**
		 * Whether a source search has gone out for this file within 'seconds'.
		 *
		 * The rotation asks this so that it does not repeat a file a caller has just
		 * asked about out of turn: the two paths bound themselves separately - one by
		 * an interval and one by a burst allowance - so without it the first file of
		 * a session is searched for twice, once by whoever wanted it and once by the
		 * rotation, whose interval has always elapsed at start-up.
		 */
		bool searchedWithin(time_t seconds);

		/** Record that a source search has just gone out for this file. */
		void noteSearched();

	public:
		inline ChunkMap& getChunks() { return chunks; }
		inline const ChunkMap& getChunks() const { return chunks; }

		bool isComplete() const;
		uint64_t getDoneBytes() const;

		/**
		 * The tiger tree leaves, once a peer has given them to us.
		 * Without them a chunk cannot be checked until the whole file is in.
		 */
		inline LeafHashes* getLeaves() { return leafHashes; }
		bool loadLeaves(const char* data, size_t length);
		bool loadLeavesFromDisk();

		/** Where the leaves are cached between runs; caller frees. */
		char* makeLeafFilename() const;

		/**
		 * Only files we are able to resume and re-source are written to disk:
		 * that means a known TTH, a known size, and not already finished.
		 *
		 * A file without a TTH is perfectly usable for the rest of this run -
		 * it just cannot be matched against a search reply or verified once
		 * it completes, so remembering it across a restart would be a promise
		 * we cannot keep.
		 */
		bool isPersistent() const;

	protected:
		/*
		 * An empty tth is one we do not have, which getTTH() reports as none:
		 * a root is always QUICKDC_TTH_LENGTH base32 characters, so there is no
		 * empty one for that to be mistaken for. The two filenames have no such
		 * reading - they are whatever we were asked to download to.
		 */
		std::string filename;               // local filename
		std::string tempname;               // local filename + ".part"
		std::string tth;                    // tth, empty if not known
		uint64_t size;                      // size of file (0 if unknown)
		Samurai::TimeStamp added;           // the time the file was queued

		/* The one hub to ask about this file; empty asks every one. */
		std::string only_hub;

		/*
		 * When a source search last went out for this file, and whether one ever
		 * did. Both, because a file that has never been searched for has no
		 * timestamp that means anything and must not be taken for one searched a
		 * moment ago - which is what a fresh TimeStamp reads as.
		 */
		Samurai::TimeStamp searched;
		bool ever_searched = false;

		std::vector<FileSource*> sources;
		std::set<FileSource*> active;       // sources with a transfer in flight
		enum Priority priority;
		ChunkMap chunks;
		LeafHashes* leafHashes;

		friend class QueueManager;
		friend class QueueXMLReader;
};


/**
 * A request for someone's file list.
 *
 * This is deliberately not a QueueFile. A file list has no TTH, its size is
 * not known up front, and its remote name depends on what the peer supports
 * (files.xml or files.xml.bz2), so there is nothing to resume and nothing
 * worth remembering across a restart. It is never written to the queue.
 */
class FilelistRequest {
	public:
		/**
		 * @param dir where the downloaded list is kept, so it can be pointed
		 *        somewhere other than the real one.
		 */
		FilelistRequest(const char* hub, const char* cid,
		                const char* dir = QUICKDC_LIST_DIR);
		virtual ~FilelistRequest();

		/**
		 * Whether an identifier is one we are willing to name a file after.
		 *
		 * The identifier is only a CID when the hub gave us one and it verified:
		 * on NMDC it is always the nickname, and on ADC it is the nickname
		 * whenever verification failed. A nickname is arbitrary bytes chosen by
		 * whoever is on the hub, and it ends up inside a path, so a user called
		 * "../../../../home/user/.config/autostart/x" would otherwise be a write
		 * wherever the hub cares to point us.
		 *
		 * Accepts what a CID or an ordinary nickname contains and nothing that
		 * can leave the list directory: no path separator, no "..", no control
		 * byte, and not empty.
		 */
		static bool isValidIdentifier(const char* id);

		/**
		 * False when the identifier was refused, in which case there is no
		 * filename and there is nothing to download into.
		 * QueueManager::requestFilelist() never hands one of these out.
		 */
		inline bool isValid() const { return !filename.empty(); }

		inline const char* getHubUrl() const { return hub.c_str(); }
		inline const char* getClientID() const { return cid.c_str(); }
		inline const Samurai::TimeStamp& getTimeAdded() const { return added; }

		inline bool isActive() const { return active; }
		inline void setActive(bool active_) { active = active_; }

		/**
		 * Where the fetched list is written; ~/.quickdc/lists/{cid}.xml
		 * The ".bz2" suffix is appended by the caller if the list arrives
		 * compressed. 0 if the identifier was refused; see isValid().
		 */
		inline const char* getLocalFilename() const { return filename.empty() ? 0 : filename.c_str(); }

		/**
		 * Where the bytes land while the list is arriving. The peer decides
		 * whether it sends files.xml or files.xml.bz2 and the protocol does
		 * not say which, so the raw download is kept aside and sniffed before
		 * it becomes getLocalFilename(). 0 if the identifier was refused.
		 */
		inline const char* getTempFilename() const { return tempname.empty() ? 0 : tempname.c_str(); }

		bool matches(const char* hub, const char* cid) const;

	public:
		/** We are about to ask someone for this list. */
		void onAttempt();

		/** Asking failed, so it will have to be tried again. */
		void onFailure();

		inline size_t getRetryCount() const { return retryCount; }

		/**
		 * Whether it is worth asking again yet.
		 *
		 * The queue is processed several times a second, so without this a
		 * user who has left the hub would be retried hundreds of times, and
		 * every attempt reports an error to the user interface.
		 */
		bool isReadyToRetry() const;

	protected:
		/*
		 * Both names are left empty when the identifier was refused, which is
		 * what isValid() reports on. Neither has an empty form of its own: they
		 * are built from a directory and an identifier that is never empty.
		 */
		std::string hub;
		std::string cid;
		std::string filename;
		std::string tempname;
		Samurai::TimeStamp added;
		Samurai::TimeStamp lastAttempt;
		size_t retryCount;
		bool active;
};


/**
 * What we have asked a single peer for, keyed by the connection token.
 *
 * A peer connection only carries the token we put in the CTM/RCM, so this is
 * how a client session finds out what it is supposed to be downloading.
 * Purely transient.
 */
class PendingRequest {
	public:
		enum Kind { KindFile, KindFilelist };

		PendingRequest(const char* token, QueueFile*, FileSource*);
		PendingRequest(const char* token, FilelistRequest*);
		virtual ~PendingRequest();

		PendingRequest(const PendingRequest&) = delete;
		PendingRequest& operator=(const PendingRequest&) = delete;

		inline const char* getToken() const { return token.c_str(); }
		inline enum Kind getKind() const { return kind; }

		/** Valid for KindFile only. */
		inline QueueFile* getFile() const { return file; }
		inline FileSource* getSource() const { return source; }

		/** Valid for KindFilelist only. */
		inline FilelistRequest* getFilelist() const { return filelist; }

		inline const Samurai::TimeStamp& getTimeAdded() const { return added; }

		/**
		 * How long ago this was asked for, in seconds.
		 *
		 * On its own clock rather than from 'added', which counts in whole seconds
		 * - and a chunk on a fast source is gone in a fraction of one.
		 */
		double elapsedSeconds() const;

		/** The chunk this peer is expected to deliver, if any. */
		void setReservation(uint64_t offset, uint64_t length);
		void clearReservation();
		inline bool haveReservation() const { return reserved; }
		inline uint64_t getReservedOffset() const { return offset; }
		inline uint64_t getReservedLength() const { return length; }

	protected:
		std::string token;
		enum Kind kind;
		QueueFile* file;
		FileSource* source;
		FilelistRequest* filelist;
		Samurai::TimeStamp added;
		std::chrono::steady_clock::time_point asked;
		bool reserved;
		uint64_t offset;
		uint64_t length;
};


/**
 * A flat snapshot of one queued file, for schedulers and user interfaces.
 *
 * The string members point into the QueueFile they were read from, so they
 * are only valid for as long as that file stays queued.
 */
struct QueueStatus {
	const char* filename;
	const char* tth;
	uint64_t size;
	uint64_t done;
	size_t sources;
	size_t activeSources;
	enum QueueFile::Priority priority;
	bool complete;
};


/**
 * Implement this and register it with QueueManager::addListener() to be told
 * about the queue as it changes. Intended for user interfaces, in the same way
 * as TransferListener.
 *
 * The bodies are empty rather than pure so that a listener only has to override
 * the events it cares about.
 *
 * Every QueueFile handed over here is only valid for the duration of the call:
 * the removal events in particular are emitted immediately before it is deleted,
 * so anything worth keeping has to be copied on the spot.
 */
class QueueListener {
	public:
		virtual ~QueueListener() { }

		/** A file was queued. */
		virtual void EventQueueFileAdded(const QueueFile*) { }

		/**
		 * A file left the queue, for whatever reason - finished, cancelled, or
		 * given up on. Always emitted, including after EventQueueFileVerified.
		 */
		virtual void EventQueueFileRemoved(const QueueFile*) { }

		/**
		 * A file downloaded in full, matched its hash, and has been moved to
		 * getLocalFilename().
		 *
		 * This is the only notification that a download succeeded. It is emitted
		 * just before the entry is removed, which is why it is separate from
		 * EventQueueFileRemoved - by the time that arrives there is no way to
		 * tell a completed download from a cancelled one.
		 */
		virtual void EventQueueFileVerified(const QueueFile*) { }

		/**
		 * A file list request ended, however it ended: downloaded, given up on,
		 * or refused. Where it arrived it is at getLocalFilename() by now.
		 * Emitted just before the request is deleted.
		 */
		virtual void EventFilelistFinished(const FilelistRequest*) { }
};

class QueueManager {
	public:
		/**
		 * @param path_ where the queue is read from and written back to, so it
		 *        can be pointed somewhere other than the real one.
		 * @param lists where requested file lists are downloaded to.
		 */
		explicit QueueManager(Core* core, const char* path_ = QUICKDC_QUEUE_FILE,
		                      const char* lists = QUICKDC_LIST_DIR);

		/** A queue run on its own, with nothing to schedule transfers through. */
		explicit QueueManager(const char* path_ = QUICKDC_QUEUE_FILE) : QueueManager(static_cast<Core*>(nullptr), path_) { }
		virtual ~QueueManager();

		QueueManager(const QueueManager&) = delete;
		QueueManager& operator=(const QueueManager&) = delete;

	public:
		/** Ends when the returned subscription is destroyed. */
		Subscription subscribe(QueueListener*);

		/** Listeners are not owned; remove them before they are destroyed. */
		void addListener(QueueListener*);
		void removeListener(QueueListener*);

		/**
		 * Do timely; expire sources and stale requests, start transfers for
		 * whatever is queued, and flush the queue to disk now and then.
		 *
		 * Called from Core::run(), so this has to stay cheap.
		 */
		void process();

	public: /* contents */

		/**
		 * Queue a file, or return the file already queued under that name.
		 * @return the queue entry, never 0.
		 */
		QueueFile* add(const char* filename, const char* tth, uint64_t size);

		/** Add a queue entry. Ownership is taken over. */
		void add(QueueFile*);

		/** Remove and delete a queue entry. */
		void remove(QueueFile*);

		size_t size() const;

		QueueFile* getFileByTTH(const char* tth) const;
		QueueFile* getFileByName(const char* localFilename) const;

		/**
		 * @return the first queued file which has a source pointing at the
		 *         given user, or 0 if we want nothing from them.
		 */
		QueueFile* getFileBySource(const char* hub, const char* cid) const;

		/* Built-in iterators */
		QueueFile* first();
		QueueFile* next();

	public: /* the scheduler API */

		/**
		 * Files that could use more sources: not paused, not complete, and
		 * currently known from fewer than 'minSources' places.
		 *
		 * This is what a scheduler walks to decide what to go searching for.
		 */
		void getFilesNeedingSources(std::vector<QueueFile*>& out, size_t minSources) const;

		/**
		 * The most deserving file that has a source we are not already
		 * downloading from.
		 *
		 * @return false if there is nothing worth starting right now.
		 */
		bool getNextTransfer(QueueFile*& file, FileSource*& source);

		/**
		 * Reserve the next chunk of 'file' for 'source'.
		 * @return false if the file has nothing left to download.
		 */
		bool reserveChunk(QueueFile* file, FileSource* source, uint64_t& offset, uint64_t& length);

		/** The chunk arrived intact. */
		void commitChunk(QueueFile* file, uint64_t offset, uint64_t length);

		/**
		 * Check a freshly written chunk against the leaf hashes.
		 * @return true if it matches, or if we have no leaves to check against.
		 */
		bool verifyChunk(QueueFile* file, uint64_t offset, uint64_t length);

		/**
		 * A peer told us which parts of a queued file it holds (PSR).
		 * @param id the CID on ADC, the nickname on NMDC.
		 */
		void onPartialSources(const char* hubUrl, const char* id, const char* tth, const char* indexes);

		/**
		 * Open a queued file's cached leaf hashes, for upload (PFSR).
		 * @return 0 if we have none; the caller owns what is returned.
		 */
		Samurai::IO::File* openLeafFile(const char* tth, uint64_t& length);

		/**
		 * Open a partially downloaded file for upload, if the range asked for
		 * is one the leaves have vouched for (PFSR).
		 *
		 * @param servable set to how far the verified run reaches.
		 * @return 0 if we cannot serve it; the caller owns what is returned.
		 */
		Samurai::IO::File* openVerifiedRange(const char* tth, uint64_t offset, int64_t length, uint64_t& servable);



		/** The chunk did not arrive; let someone else have another go. */
		void releaseChunk(QueueFile* file, uint64_t offset, uint64_t length);

		bool getStatus(QueueFile*, QueueStatus& out) const;
		void getStatus(std::vector<QueueStatus>& out) const;

		size_t countActiveTransfers() const;

		/** The chunk size we hand out, from the configuration. */
		uint64_t getChunkSize() const;

		/**
		 * How much this source would be asked for next.
		 *
		 * Chunking costs a round trip per chunk, which is nothing on a slow source
		 * and a great deal on a fast one: at twenty five megabytes a second a
		 * megabyte is gone in forty milliseconds, so a connection asked for a
		 * megabyte at a time spends a good part of its life waiting to be told what
		 * to fetch next, and the rate sawtooths. So the size follows the source -
		 * about QUICKDC_CHUNK_TARGET_SECONDS of transfer at whatever it has been
		 * managing - which puts that cost back into the noise.
		 *
		 * The configured chunk size is the floor rather than the answer: it is what
		 * a source is asked for until it has shown what it can do, and it is never
		 * gone below. QUICKDC_CHUNK_SIZE_MAX is the ceiling.
		 *
		 * @param source may be null, which yields the configured size.
		 */
		uint64_t chunkSizeFor(const FileSource* source) const;

	public: /* sources for a file somebody is waiting on */

		/**
		 * What came of asking for sources for one file.
		 *
		 * A caller wants to tell these apart: a hash that is not queued is a
		 * mistake on its part, while a refused rate or a hub that is not logged in
		 * yet is only a reason to ask again in a moment. Note that the last of them
		 * still costs an ask: the allowance is charged before there is any way of
		 * knowing whether a search can leave.
		 */
		enum SourceSearchResult {
			SourceSearchSent,         /* the search went out */
			SourceSearchBadHash,      /* not a TTH root at all */
			SourceSearchNotQueued,    /* nothing is queued under that hash */
			SourceSearchRateLimited,  /* asked for too often; try again shortly */
			SourceSearchNotSent,      /* nowhere to send it; no hub is logged in */

			/*
			 * This file names one hub - see QueueFile::searchOnlyOn() - and it is not
			 * one we are logged in to.
			 *
			 * Apart from NotSent because the two are not the same condition and the
			 * rotation must not treat them alike: nothing can be sent anywhere is a
			 * reason to stop the pass, while this one file having nowhere to go is a
			 * reason to move on to the next.
			 */
			SourceSearchHubAbsent
		};

		/**
		 * Search every hub for sources for one queued file now, rather than
		 * waiting for its turn in the rotation.
		 *
		 * searchForSources() sends one search per QUICKDC_SOURCE_SEARCH_INTERVAL,
		 * taking the files that are short of sources in turn. That is the right
		 * shape for a download queue: nobody is watching it, and being tolerable
		 * to the hub matters more than how quickly any one file finds its second
		 * source. It is useless for a document somebody is waiting to read - ten
		 * of them would be twenty minutes before the last was even looked for, and
		 * every one of those turns is one the user's own downloads did not get.
		 * So queueing a file is not enough on its own when a reader is waiting:
		 * ask for this as well.
		 *
		 * Bounded by an allowance of QUICKDC_SOURCE_SEARCH_BURST asks, credited
		 * back one per QUICKDC_SOURCE_SEARCH_BURST_REFILL seconds, so that a
		 * caller cannot turn this into the search flood a hub disconnects clients
		 * for. The allowance is spent on the asking rather than on the sending, so
		 * that a caller which keeps asking while no hub is logged in is bounded
		 * too; the search it did not get is one the rotation will make eventually.
		 *
		 * The rotation is left strictly alone. This neither delays it nor takes a
		 * turn from it, so the rest of the queue is searched for on exactly the
		 * schedule it would have had.
		 *
		 * Called from the thread that drives the Core, like the rest of this class.
		 *
		 * @param why if given, set to the outcome whether or not it succeeded.
		 * @return false unless a search actually went out.
		 */
		bool requestSourceSearch(const char* tth, SourceSearchResult* why = 0);

		/**
		 * Add a source for a queued file by hash and user, without searching.
		 *
		 * A search is how sources are found for a file that came from nowhere in
		 * particular, and it is not needed when one of them is already named. A
		 * BBS0 index entry carries the CID of the session the hub accepted the
		 * post from, so the author of a post is a known source for its body from
		 * the moment the entry arrives - and a client that can reach nobody else
		 * can always ask them.
		 *
		 * The source is recorded as {hubUrl}/{cid}/TTH:{root}, which is the URL
		 * notation filesource.h describes and the same spelling a search reply and
		 * a PSR are turned into. Note that this is not what goes on the wire:
		 * both protocols ask for "TTH/{root}", built from the QueueFile's own hash
		 * rather than from the source, so the source's filename only decides
		 * whether it names a file at all - one with no filename is a request for
		 * the peer's file list, see FileSource::isFilelist(). That is why this
		 * spells the hash out instead of leaving the filename off.
		 *
		 * Called from the thread that drives the Core, like the rest of this class.
		 *
		 * @return false if the hash is not a TTH root, if no file with that hash
		 *         is queued, if either identifier is unusable, or if that source
		 *         is one we already know about.
		 */
		bool addKnownSource(const char* tth, const char* hubUrl, const char* cid);

		/**
		 * What a source on that hub is to be recorded under: a SID given on ADC
		 * becomes the CID of the user who has it there, so a peer is one source
		 * however it was named. What the hub cannot place is returned as given.
		 */
		std::string sourceIdOn(const char* hubUrl, const char* id) const;

	public: /* file lists - transient, never stored */

		/**
		 * Ask for a user's file list, or return the request already pending
		 * for that user.
		 */
		/**
		 * Ask for a user's file list, or return the request already pending
		 * for that user.
		 *
		 * Only queues it; process() makes the connection, so a request that
		 * cannot be served right now is retried rather than lost.
		 *
		 * @return 0 if the identifier is not one we can name a file after; see
		 *         FilelistRequest::isValidIdentifier().
		 */
		FilelistRequest* requestFilelist(const char* hub, const char* cid);
		FilelistRequest* getFilelistRequest(const char* hub, const char* cid) const;

		/** Enumerate the pending requests, for a user interface. */
		FilelistRequest* getFilelistRequest(size_t n) const;

		void removeFilelistRequest(FilelistRequest*);
		size_t countFilelistRequests() const;

	public: /* token correlation, used by the client sessions */

		/**
		 * Note that we have asked a peer for something, and return the token
		 * to put in the connection request. The returned string is owned by
		 * the queue and stays valid until the request is resolved.
		 */
		const char* createRequest(QueueFile*, FileSource*);
		const char* createRequest(FilelistRequest*);

		/**
		 * What did we want from the peer that turned up with this token?
		 *
		 * The returned pointer is only good until the queue is next changed,
		 * so callers should hold on to the token and look it up again rather
		 * than caching this.
		 */
		PendingRequest* lookupRequest(const char* token) const;

		/**
		 * Find the request we are waiting on from a given user.
		 *
		 * An NMDC peer connection carries no token - the protocol has no room
		 * for one - so which user we are talking to is the only handle there
		 * is. Returns 0 if we want nothing from them.
		 */
		PendingRequest* lookupRequestBySource(const char* hub, const char* cid) const;

		/** The peer let us down; release the chunk and blame the source. */
		void onRequestFailed(const char* token);

		/** We got what we asked for. */
		void onRequestFinished(const char* token);

	public:
		/**
		 * A file finished downloading and has been hashed; compare it against
		 * what we expected and either keep it or start over.
		 * Called by QueueVerifier, deferred to process().
		 */
		void onFileVerified(const char* tempname, const char* tth);

		/**
		 * A requested file list has arrived in full at getTempFilename().
		 * Unpacks it if it is compressed, moves it into place, reads it, and
		 * turns anything the queue is waiting for into a new source.
		 *
		 * Shared by both protocols, since neither the compression sniffing nor
		 * the source matching is protocol specific.
		 *
		 * @param hub the hub URL a resulting FileSource should record.
		 * @param cid the user's CID (ADC) or nick (NMDC).
		 * @return the number of new sources it gave us.
		 */
		size_t onFilelistDownloaded(FilelistRequest* request, const char* hub, const char* cid);

		bool load();
		bool save();

	protected:
		/**
		 * Ask peers for the work that is queued. This is deliberately simple;
		 * it is the seam a dedicated scheduler would take over.
		 */
		void schedule();

		/** Every chunk is in; hand the file over for hash verification. */
		void onFileComplete(QueueFile*);

		void expire();
		void processVerifications();

		/** Search for more sources for one file that is short of them. */
		void searchForSources();

		/**
		 * Ask every hub where a file can be had, and remember the token the
		 * replies will quote.
		 *
		 * Shared by the rotation and by requestSourceSearch(), which differ only in
		 * what they are allowed to send and how often - the search itself is the
		 * same one either way.
		 */
		enum SourceSearchResult sendSourceSearch(QueueFile* file);

		/**
		 * Take one search from the burst allowance.
		 * @return false if there is none left to take; see requestSourceSearch().
		 */
		bool takeSearchFromBurst();

		/** @return false if this source holds nothing we still need. */
		bool sourceIsUseful(QueueFile* file, FileSource* source) const;

		/** Whether a request on this file is already out to the peer this source names. */
		bool isPeerBusyWith(QueueFile* file, FileSource* source) const;

		void clearRequestsFor(QueueFile*);
		void removeRequest(const char* token);

		/**
		 * Recursively turn file list entries into sources for queued files.
		 * Note the leading '::': the file list types live in a top level
		 * 'Share' namespace, not in QuickDC::Share.
		 *
		 * @param depth how far down the peer's tree this call already is;
		 *        the walk stops at QUICKDC_FILELIST_MAX_DEPTH.
		 */
		size_t collectSources(::Share::RemoteDir* dir, const char* hub, const char* cid,
		                      size_t depth = 0);

		/** A token unique to this client, safe to put on the wire. */
		std::string makeToken();

		/** How many peers we ask for data at the same time. */
		size_t getMaxDownloads() const;

	protected:
		/** Null for a queue run on its own, which then schedules nothing. */
		Core* core;
		ListenerList<QueueListener> listeners;
		std::vector<QueueFile*> files;
		std::vector<QueueFile*>::iterator fileIterator;
		std::vector<FilelistRequest*> filelists;
		std::map<std::string, PendingRequest*> pending;

		/** Hash results, applied in process() so we never mutate the queue
		    from inside a hash manager callback. */
		struct Verification {
			std::string tempname;
			std::string tth;
		};
		std::vector<Verification> verifications;

		QueueVerifier* verifier;
		bool dirty;
		size_t tokenCounter;
		Samurai::TimeStamp lastSaved;

		Samurai::TimeStamp lastSourceSearch;
		size_t searchRotation;

		/* The burst requestSourceSearch() spends, kept apart from the rotation
		   above so that neither can hold the other up. */
		Samurai::TimeStamp lastBurstSearch;
		size_t burstAllowance;

		std::string path;      // the queue on disk
		std::string pathNew;   // written first, then moved into place
		std::string listDir;
};


}

#endif // HAVE_QUICKDC_DOWNLOADQUEUE_H
