/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DOWNLOADQUEUE_H
#define HAVE_QUICKDC_DOWNLOADQUEUE_H

#include "quickdc.h"
#include "network/chunkmap.h"
#include <string.h>
#include <stdlib.h>
#include <samurai/timestamp.h>
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
#define QUICKDC_DOWNLOAD_DIR   "~/.quickdc/downloads"

/** How much we ask a single source for at a time, unless configured otherwise. */
#define QUICKDC_CHUNK_SIZE (1024*1024)

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

/** A TTH root in base32 is always this long. */
#define QUICKDC_TTH_LENGTH 39

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

		static const char* convertPriority(enum Priority);
		static enum Priority convertPriority(const char*);

	public:
		/** The file we are downloading to, once it is finished. */
		inline const char* getLocalFilename() const { return filename; }

		/** The file we are downloading into while incomplete. */
		inline const char* getTempFilename() const { return tempname; }

		/** The TTH root in base32, or 0 if we do not know it. */
		inline const char* getTTH() const { return tth; }

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
		char* filename;                     // local filename
		char* tempname;                     // local filename + ".part"
		char* tth;                          // tth
		uint64_t size;                      // size of file (0 if unknown)
		Samurai::TimeStamp added;           // the time the file was queued
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
		FilelistRequest(const char* hub, const char* cid);
		virtual ~FilelistRequest();

		inline const char* getHubUrl() const { return hub; }
		inline const char* getClientID() const { return cid; }
		inline const Samurai::TimeStamp& getTimeAdded() const { return added; }

		inline bool isActive() const { return active; }
		inline void setActive(bool active_) { active = active_; }

		/**
		 * Where the fetched list is written; ~/.quickdc/lists/{cid}.xml
		 * The ".bz2" suffix is appended by the caller if the list arrives
		 * compressed.
		 */
		inline const char* getLocalFilename() const { return filename; }

		/**
		 * Where the bytes land while the list is arriving. The peer decides
		 * whether it sends files.xml or files.xml.bz2 and the protocol does
		 * not say which, so the raw download is kept aside and sniffed before
		 * it becomes getLocalFilename().
		 */
		inline const char* getTempFilename() const { return tempname; }

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
		char* hub;
		char* cid;
		char* filename;
		char* tempname;
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

		inline const char* getToken() const { return token; }
		inline enum Kind getKind() const { return kind; }

		/** Valid for KindFile only. */
		inline QueueFile* getFile() const { return file; }
		inline FileSource* getSource() const { return source; }

		/** Valid for KindFilelist only. */
		inline FilelistRequest* getFilelist() const { return filelist; }

		inline const Samurai::TimeStamp& getTimeAdded() const { return added; }

		/** The chunk this peer is expected to deliver, if any. */
		void setReservation(uint64_t offset, uint64_t length);
		void clearReservation();
		inline bool haveReservation() const { return reserved; }
		inline uint64_t getReservedOffset() const { return offset; }
		inline uint64_t getReservedLength() const { return length; }

	protected:
		char* token;
		enum Kind kind;
		QueueFile* file;
		FileSource* source;
		FilelistRequest* filelist;
		Samurai::TimeStamp added;
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


class QueueManager {
	public:
		QueueManager();
		virtual ~QueueManager();

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

		/** @return false if this source holds nothing we still need. */
		bool sourceIsUseful(QueueFile* file, FileSource* source) const;

		void clearRequestsFor(QueueFile*);
		void removeRequest(const char* token);

		/**
		 * Recursively turn file list entries into sources for queued files.
		 * Note the leading '::': the file list types live in a top level
		 * 'Share' namespace, not in QuickDC::Share.
		 */
		size_t collectSources(::Share::RemoteDir* dir, const char* hub, const char* cid);

		/** A token unique to this client, safe to put on the wire. */
		std::string makeToken();

		/** How many peers we ask for data at the same time. */
		size_t getMaxDownloads() const;

	protected:
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
};


}

#endif // HAVE_QUICKDC_DOWNLOADQUEUE_H
