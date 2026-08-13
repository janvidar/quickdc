/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_BOARDINDEX_H
#define HAVE_QUICKDC_BOARDINDEX_H

#include "quickdc.h"

#include "api/bulletins.h"

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace QuickDC {

/**
 * What a hub said is on a board, kept so that a board is there before the hub is.
 *
 * One file per board per hub, appended a record at a time and never rewritten.
 * An index only grows at its end - an entry is immutable except for withdrawal,
 * and a withdrawal is itself an append - and an append is the one write that
 * leaves a reader either the whole of the old file or the whole of the new one,
 * so a client that goes down mid-write costs the last entry the hub sent rather
 * than every entry it ever sent.
 *
 * Nothing is dropped as it ages. Both halves of a subscription arrive as one
 * stream and are indistinguishable, so what is held is what has been seen, and
 * cursor() is where a subscription picks the stream up again. A hub that sends
 * entries for ever therefore grows this file for ever, which is deliberate: the
 * bound a client is asked to keep is on the documents, which are as large as their
 * authors made them, and not on the entries, which are a few hundred bytes each -
 * see PostStore::Bounds.
 *
 * Entries are scoped to the hub that sent them, and that is why the key is a hub
 * and a board rather than a board alone. The board, the timestamp, the thread
 * root and the attested CID are one hub's assertions about one board: merging two
 * hubs' assertions produces a board that exists nowhere. A post document, by
 * contrast, is named by the hash of its own bytes and is the same post on every
 * hub that indexes it, which is why the documents are held somewhere else - see
 * bbs/poststore.h.
 *
 * The file is named after a digest of the key and never after the names in it. A
 * board name is protocol text a hub chooses, and the character set that
 * ADC::isValidBoardName() admits excludes the path separators but permits '.', so
 * ".." is a legal board name and so is "...". Every file here is therefore named
 * in characters this client picked.
 *
 * The digest is short and is not collision resistant, and does not have to be,
 * because nothing is merged on the strength of it: each file also carries the hub
 * and board it was made for, and a file whose key is not the one being asked for
 * is not this board's index and is left alone. Two keys that land on one name
 * cost the second of them its cache rather than giving it the first one's entries
 * - which matters here in a way it does not for a chat transcript, since a hub
 * chooses its own board names and could otherwise aim one at another hub's file.
 *
 * Nothing here is evidence. Every record is one hub's claim, kept as it was made;
 * see api/bulletins.h for what may and may not be shown to a user on the strength
 * of it. Like the transcripts beside them these files are in the clear, which is
 * the same standing as the download queue and the hub passwords: worth saying out
 * loud rather than leaving to be discovered.
 */
class BoardIndex {

	public:
		/**
		 * Whether what is held for a board can still be caught up with.
		 *
		 * A hub replays no further back than the OT of its board descriptor, and
		 * says so in advance precisely so that a client can tell these apart. It
		 * is a third state and not a flag on the other two, because a board no
		 * descriptor has arrived for is not a board known to be whole.
		 */
		enum class View {
			/** No descriptor has been seen, so the question cannot be answered. */
			Unknown,

			/** Everything before what is held will still be replayed. */
			Complete,

			/**
			 * The cursor is older than the oldest timestamp the hub will replay,
			 * so the entries between the two are not coming and this view of the
			 * board has a hole in it that no subscription will close.
			 */
			Gap
		};

		/**
		 * @param directory where the cached indexes live, "~" resolved as
		 *        everywhere else in the core. Made if it is not there, every
		 *        component of it, and every call is a no-op if it cannot be.
		 */
		explicit BoardIndex(const std::string& directory);

		BoardIndex(const BoardIndex&) = delete;
		BoardIndex& operator=(const BoardIndex&) = delete;

		/**
		 * Write one index entry down, unless what is held is already at least as
		 * new.
		 *
		 * This is the whole of the rule a resumed subscription needs, and it is
		 * one rule rather than two: an entry arriving for a hash already held
		 * replaces it where the arriving timestamp is greater, and is discarded
		 * where it is not. A duplicate from the resumption window has an equal
		 * timestamp and goes; a tombstone has a greater one and takes effect.
		 *
		 * The entry's own 'board' is not consulted. The file is the board, so an
		 * entry reads back on the board it was written to and no record can claim
		 * to be somewhere else.
		 *
		 * @return false when the entry was already held and nothing was written.
		 *         A caller updating a view of the board should ignore it: what it
		 *         is showing is this entry already. An entry whose hash is not a
		 *         tiger tree root is refused the same way, since it names no
		 *         document, could never be fetched, and must not reach a filename.
		 *         An index that cannot be written at all returns true, because
		 *         nothing is kept, so nothing can be a repeat of anything - and a
		 *         caller deciding whether to show an entry must not be told to
		 *         drop a live one.
		 */
		bool append(const std::string& hubUrl, const std::string& board,
		            const BoardEntry& entry);

		/**
		 * Everything held for a board, oldest first, one entry per hash.
		 *
		 * The whole file, not its recent end: a board is read to be displayed as a
		 * board, so there is no useful tail of one, and the file is bounded by
		 * what a hub has accepted rather than by how long this client has been
		 * running.
		 *
		 * A withdrawn post is here as its tombstone, carrying its hash, its
		 * timestamp and nothing else. Dropping it would be a client that keeps
		 * being told to forget a post it has already forgotten, and it would
		 * resurrect the post on the next full re-subscription.
		 *
		 * Ordered by the timestamp the hub assigned, stably, so that entries
		 * sharing a timestamp keep the order they arrived in and a thread does not
		 * rearrange itself between sessions.
		 */
		std::vector<BoardEntry> entries(const std::string& hubUrl,
		                                const std::string& board) const;

		/**
		 * Where a subscription resumes: the highest timestamp seen, and not one
		 * past it.
		 *
		 * "Highest plus one" is the natural thing to write here and it is wrong.
		 * Timestamps are seconds and are not unique, so a post the hub accepted in
		 * the same second as the last entry received would fall behind a cursor
		 * advanced past it and would never be delivered to anybody. The cost of
		 * getting it right is that the entries of that one second arrive a second
		 * time, which is what append() is written to absorb.
		 *
		 * @return 0 when nothing is held, which asks the hub for the whole board.
		 */
		uint64_t cursor(const std::string& hubUrl, const std::string& board) const;

		/**
		 * Record what the hub says the board is, as of now.
		 *
		 * Only Board::oldest is kept, because it is the only field of a descriptor
		 * that says anything about what is held here: a hub sends a fresh
		 * descriptor whenever any field of a board changes, and the rest of them
		 * describe the board rather than this client's view of it. Kept across
		 * restarts so that view() can be asked before a hub has been reached,
		 * which is when an interface is first drawn.
		 */
		void describe(const std::string& hubUrl, const Board& board);

		/**
		 * Whether what is held can still be caught up with; see View.
		 *
		 * Decided from the record and not from whether this client happened to
		 * stay connected. A hub that raises OT past the newest entry of a quiet
		 * board while this client is subscribed has cost it nothing and is still
		 * reported as a gap, because nothing on disk can tell that case from an
		 * absence - and overstating a gap costs a line in an interface, while
		 * understating one presents an incomplete board as the whole of it.
		 *
		 * A board that has never been subscribed to reads as a gap wherever the
		 * hub declares an OT above zero, and that is the honest answer rather than
		 * an artefact: the entries before OT are not coming, and a client that has
		 * none of them has a view of the board that begins in the middle.
		 */
		View view(const std::string& hubUrl, const std::string& board) const;

		/** Whether anything has been written for this board on this hub. */
		bool contains(const std::string& hubUrl, const std::string& board) const;

		/** Throw a board's cached index away. Documents are not touched. */
		void forget(const std::string& hubUrl, const std::string& board);

		/** Where a board's index is. Public so a test can look. */
		std::string pathFor(const std::string& hubUrl,
		                    const std::string& board) const;

		/** Whether the directory could be made; false makes every call do nothing. */
		bool isUsable() const;

	private:
		/** What is held in memory for one board's index; see 'cached'. */
		struct Cached {
			/** The timestamp held for each hash, which is all append() decides on. */
			std::map<std::string, uint64_t> held;

			/** The highest of them, which is the cursor. */
			uint64_t highest = 0;

			/** OT as the hub last declared it, and whether it ever did. */
			uint64_t oldest = 0;
			bool described = false;

			/** Whether the file says which hub and board it is for. */
			bool keyed = false;

			/**
			 * Whether the file's last record ends where a record should.
			 *
			 * A process that went down mid-write leaves a line with no newline on
			 * it, and the next record appended would join onto it and be lost with
			 * it. The newline is put back instead, so a crash costs the record it
			 * interrupted and not the one after it as well.
			 */
			bool terminated = true;

			/**
			 * The file says it is for another hub or another board, so it is
			 * somebody else's and nothing is read from or written to it.
			 */
			bool foreign = false;
		};

		/** Fills the entry for a board from what is on disk, once. */
		Cached& remember(const std::string& hubUrl, const std::string& board) const;

		/** Writes one record, and the key record first where there is none. */
		bool write(const std::string& path, Cached& state,
		           const std::string& hubUrl, const std::string& board,
		           const std::string& record);

		std::string directory;
		bool usable;

		/**
		 * What is held for each board, by hub and board rather than by the file.
		 *
		 * Held rather than read per entry, because a resumed subscription arrives
		 * as a burst of thousands and reading the file back for each one of them
		 * would be the whole of a board read once per entry in it.
		 *
		 * By the key and not by the path, because two keys landing on one digest
		 * must not share one state in memory any more than they may share the file:
		 * the file says whose it is, and each of them has to be able to ask.
		 */
		mutable std::map<std::string, Cached> cached;
};

}

#endif // HAVE_QUICKDC_BOARDINDEX_H
