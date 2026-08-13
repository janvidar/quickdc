/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_POSTSTORE_H
#define HAVE_QUICKDC_POSTSTORE_H

#include "quickdc.h"

#include "api/attachments.h"

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace QuickDC {

/**
 * The post documents this client holds, by hash and by nothing else.
 *
 * A Store and not a Cache, and the noun is the point. Throwing a cached file away
 * costs the next reader a fetch; throwing a post document away withdraws this
 * client as a source for it, which is a visible act on a network where the post
 * may have no other copy left. A board is durable because the people who have
 * read a post go on serving it, so what is discarded here can be the last copy of
 * something somebody wrote - and the author of a post that nobody kept has
 * published nothing. Everything below is shaped by that asymmetry: documents leave
 * this store deliberately, one at a time, and never as a side effect.
 *
 * A document is named by the tiger tree hash of its own bytes and by nothing else,
 * so the store is keyed globally rather than per hub. A post carries no board and
 * no thread root: the same bytes are the same post on every hub that indexes them,
 * and a client that fetched one from one hub already holds it for the next. The
 * hub's assertions about a board are somewhere else, per hub, in bbs/boardindex.h.
 *
 * Filenames are the hash, which is 39 characters of base32 this client has
 * checked. Nothing a hub said reaches a path: not a board name, of which ".." is a
 * legal one, and not the 'dn' of an attachment magnet. A hash that is not a tiger
 * tree root has no path at all here, and every call naming one does nothing.
 *
 * What is held is recorded in a journal beside the documents, appended a record at
 * a time and never rewritten, the last record for a hash being the one that counts.
 * The store keeps its own records rather than leaning on the configuration's
 * [Attachments] group, because whether a document is offered to peers is the
 * caller's decision to make again at every start-up - a document remembered under
 * [Attachments] would be offered again whatever that decision turned out to be,
 * and the configuration would grow a line for every post ever read.
 *
 * Three things this deliberately does not do. It does not fetch: the bytes arrive
 * through the download machinery and are handed over with adopt(). It does not
 * verify: hashing what arrived and parsing its header belong to whoever fetched
 * it, and the outcome is recorded here with record(). And it knows nothing about
 * hubs or transfers, which is why the last step of discarding a document is the
 * caller's - see release().
 *
 * Not thread safe. Belongs to the thread that drives the Core, which is the thread
 * the share manager reports a hash on.
 */
class PostStore : public Share::AttachmentListener {

	public:
		/**
		 * What became of verifying a document against the hash it was asked for.
		 *
		 * Kept with the document and keyed by its hash, because that is what
		 * verification is about: the bytes either hash to the name they were
		 * requested under and parse as a post document, or they do not, and
		 * neither answer changes from one hub or one board to the next.
		 *
		 * Failed is remembered rather than treated as absent so that a document
		 * whose bytes are not the post they claim to be is not fetched, checked
		 * and refused over and over.
		 */
		enum class Verification {
			/** Held, and nobody has said yet whether it is what it claims to be. */
			Unknown,

			/** It hashes to its name and its header parses. Only these are served. */
			Verified,

			/** It does not, and it is not the post that hash names. */
			Failed
		};

		/**
		 * What the caller is willing to hold, as evict() reads it.
		 *
		 * Both bounds are applied to documents and neither to index entries. An
		 * entry is a few hundred bytes and a board without its entries is not a
		 * board, while a document is as large as its author made it: bounding the
		 * bytes means bounding the documents, and there is nothing to be saved by
		 * forgetting what a board consists of.
		 */
		struct Bounds {
			/** Seconds since a document was adopted, after which it may go. 0: no bound. */
			uint64_t age = 0;

			/** The most, in bytes, to hold across every board. 0: no bound. */
			uint64_t total = 0;
		};

		/**
		 * @param directory where the documents and the journal live, "~" resolved
		 *        as everywhere else in the core. Made if it is not there, every
		 *        component of it, and every call is a no-op if it cannot be.
		 * @param shares what offers a document to peers, or null in a client that
		 *        has no share manager. Without one the store holds documents and
		 *        serves none of them, which is a usable client and not a broken
		 *        one: reading a board does not require answering for it.
		 */
		PostStore(const std::string& directory, Share::Manager* shares);

		~PostStore() override;

		PostStore(const PostStore&) = delete;
		PostStore& operator=(const PostStore&) = delete;

		/**
		 * Take a fetched document into the store, under the hash it was asked for.
		 *
		 * The file is moved rather than copied where the filesystem allows it, so
		 * the caller's temporary is gone either way and the bytes are not on disk
		 * twice.
		 *
		 * Nothing is verified here and the hash is taken on trust as a name. A
		 * caller that has not checked that these bytes hash to 'tth' has stored a
		 * file under a name that does not describe it, and record() is where it
		 * says which of those happened.
		 *
		 * @param tth the tiger tree root the document was requested by, base32.
		 * @param path the file as it was fetched, which this takes away.
		 * @return false when there is nothing to work with: an unusable store, a
		 *         hash that is not a tiger tree root, or a file that cannot be
		 *         read. A document already held is left alone and answers true,
		 *         because a document is immutable and the copy in hand is the same
		 *         bytes as the copy on disk.
		 */
		bool adopt(const std::string& tth, const std::string& path);

		/**
		 * Record what verifying the document came to; see Verification.
		 *
		 * A document that stops being verified stops being offered, and that is
		 * not done here: it is a detach(), so it waits for release() like every
		 * other withdrawal.
		 */
		void record(const std::string& tth, Verification outcome);

		/** What verifying it came to, or Unknown for a document that is not held. */
		Verification verification(const std::string& tth) const;

		/** Whether the document is held: recorded, and still on disk. */
		bool has(const std::string& tth) const;

		/**
		 * Where the document is, or would be. Derived from the hash alone.
		 *
		 * @return an empty string for a hash that is not a tiger tree root, which
		 *         is the only kind of name that reaches this directory.
		 */
		std::string pathFor(const std::string& tth) const;

		/** Every document held, in the order they were adopted. */
		std::vector<std::string> documents() const;

		/**
		 * What the documents come to in bytes, as they were adopted.
		 *
		 * A document marked to go counts until it is released, because until then
		 * it is still on disk and is what a bound is a bound on.
		 */
		uint64_t bytes() const;

		/**
		 * Offer the document to any peer that asks for it by exact hash.
		 *
		 * This is what makes a board durable: a post ends up held by everybody who
		 * read it and survives the departure of its author. It is offered by hash
		 * alone - no file list entry, no match for a search by name, size or
		 * extension - which discloses nothing the board did not, since the hash is
		 * on a board that every subscriber can read.
		 *
		 * Refusable, and refused by default, because serving is a statement: it
		 * says "I have read this" to anybody who asks. The caller decides, per hub
		 * and per the user's setting, and nothing is offered until it does - a
		 * document held from a hub the user has not joined is read and not served.
		 *
		 * A document that has not verified is never offered, whatever the caller
		 * asks for. Answering for bytes that are not the post they claim to be
		 * would spread a forgery under a name other clients have checked.
		 *
		 * @return false when it is not held, has not verified, or there is no share
		 *         manager to offer it with. True means it has been handed over;
		 *         hashing is asynchronous, so a peer can be served from it a
		 *         moment later rather than at once.
		 */
		bool serve(const std::string& tth);

		/**
		 * Stop offering the document, while going on holding it.
		 *
		 * The user turning serving off for a hub, or leaving one. It marks and does
		 * not detach, for the reason release() gives.
		 *
		 * @return false when it was not being offered in the first place.
		 */
		bool withhold(const std::string& tth);

		/** Whether the document has been offered to peers and not yet withdrawn. */
		bool isServed(const std::string& tth) const;

		/**
		 * Mark a document to be thrown away: a post that has been withdrawn.
		 *
		 * A tombstone is a request, honoured by the clients that receive it and
		 * meaningless to those that do not, so this is not deletion and must never
		 * be presented to a user as a guarantee that a post is gone. Every other
		 * reader that fetched it holds it too, and goes on serving it.
		 *
		 * It marks and does not delete; release() is the other half.
		 *
		 * @return false when the document is not held.
		 */
		bool forget(const std::string& tth);

		/**
		 * Choose the documents that should go, by age and by total size, and mark
		 * them.
		 *
		 * The oldest adoption first, until what is left is inside 'bounds'. A
		 * document already marked counts as gone and is not chosen again.
		 *
		 * It marks and does not delete, which is what makes eviction safe to run
		 * from a timer: see release().
		 *
		 * @param now seconds since the epoch, by which age is judged.
		 * @return the documents this call marked, which the caller is now expected
		 *         to release() as it can vouch for each of them.
		 */
		std::vector<std::string> evict(const Bounds& bounds, uint64_t now);

		/**
		 * Every document waiting on a release(): marked to go, or withheld from
		 * being offered any longer.
		 *
		 * Marks survive a restart, and at a restart nothing is being served,
		 * because an offer lasts only as long as the process that made it. A client
		 * starting up can therefore release everything this returns without asking
		 * anybody, which is the one moment that vouching is free.
		 */
		std::vector<std::string> pending() const;

		/**
		 * Carry out what forget(), evict() or withhold() decided: stop offering the
		 * document, and delete it where it was marked to go.
		 *
		 * The caller has to say when, and that is not squeamishness. An upload
		 * takes the interior handle Share::Manager::getFileByTTH() returns - a
		 * member of the object detach() destroys - and reads through it until the
		 * transfer ends, so detaching a file a peer is being sent leaves that
		 * transfer reading freed memory. The share manager is told nothing about
		 * transfers and this store knows nothing about either, so neither of them
		 * can tell. The interface layer can: it is where the transfers in flight
		 * are already listed, so that is where the assurance comes from.
		 *
		 * Hence the shape of every withdrawal here. Choosing is separate from
		 * doing, choosing is safe at any moment, and doing waits for a caller that
		 * can say nothing is reading the file. A caller that cannot say so yet
		 * leaves the document marked and calls this later; nothing is lost by
		 * waiting, since a marked document is still readable and still correct.
		 *
		 * @return false when the document is not held, or when nothing was pending
		 *         for it.
		 */
		bool release(const std::string& tth);

		/** Whether the directory could be made; false makes every call do nothing. */
		bool isUsable() const;

		/** Where the journal of what is held is. Public so a test can look. */
		std::string journalPath() const;

	protected:
		/**
		 * Share::AttachmentListener. The store is its own listener because it is
		 * the only party that knows which hash the file was supposed to have: the
		 * share manager hashes what it is given and says what it found, and a file
		 * whose contents do not hash to the name it is filed under is not the post
		 * that name refers to, however it came to be there.
		 */
		void EventAttachmentReady(const char* path, const char* tth, uint64_t size) override;
		void EventAttachmentFailed(const char* path, const char* reason) override;

	private:
		/** What is held for one document; a line of the journal, and its state. */
		struct Held {
			std::string tth;

			/** Its length in bytes, as it was adopted. */
			uint64_t size = 0;

			/** When it was adopted, in seconds since the epoch; evict() judges by it. */
			uint64_t adopted = 0;

			Verification verified = Verification::Unknown;

			/** Marked to go by forget() or evict(), and waiting on a release(). */
			bool leaving = false;

			/**
			 * Offered to peers, and asked to stop being offered.
			 *
			 * Neither is journalled, because neither outlives the process: an offer
			 * is made to the share manager for this session only, so a store that
			 * has just started is offering nothing and has nothing to withdraw.
			 */
			bool offered = false;
			bool withheld = false;
		};

		/** Reads the journal, once, dropping records whose document has gone. */
		void remember() const;

		/** Appends the current state of one document to the journal. */
		bool write(const Held& document);

		Held* find(const std::string& tth) const;

		std::string directory;
		bool usable;
		Share::Manager* shares;

		/**
		 * What is held, by hash, and the order it was adopted in.
		 *
		 * Held rather than read per question, because a client re-offers what it
		 * has at start-up and asks about every document of a board as it draws it.
		 * The order is kept beside the map because it is what eviction chooses by,
		 * and a map of hashes is in no useful order at all.
		 */
		mutable std::map<std::string, Held> held;
		mutable std::vector<std::string> order;
		mutable bool loaded;

		/**
		 * Whether the journal's last record ends where a record should.
		 *
		 * A process that went down mid-write leaves a line with no newline on it,
		 * and the next record appended would join onto it and be lost with it. The
		 * newline is put back instead, so a crash costs the record it interrupted
		 * and not the one after it as well.
		 */
		mutable bool terminated;
};

}

#endif // HAVE_QUICKDC_POSTSTORE_H
