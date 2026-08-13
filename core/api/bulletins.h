/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_BULLETINS_API_H
#define HAVE_QUICKDC_BULLETINS_API_H

/**
 * Bulletin boards: what a hub says there is to read, and what a post says it is.
 *
 * A facade, for the same reason as api/attachments.h: a user interface includes
 * core/api/ and nothing else. It is also the one place the BBS0 value types are
 * written down, so that the protocol code that reads them off the wire, the
 * storage that keeps them, the hub that serves them and the Qt bridge that
 * displays them are all talking about the same board and the same post.
 *
 * BBS0 rests on a single split, and the shape of everything here follows from it:
 *
 *  - a post is a file, named by the tiger tree hash of the whole document: the
 *    header, the newline that ends it, and every byte of the body. Nobody can
 *    change the author, the parent, the subject or the content without changing
 *    the post's name, and that includes the hub.
 *  - the hub holds an index and no bytes. Which board a post is on, when it
 *    arrived and what nickname its author held are the hub's to say, and it says
 *    them in a BoardEntry.
 *  - the bytes travel between clients, fetched by hash over the search and
 *    transfer machinery ADC already has, exactly as any other file.
 *
 * Six things a reader of this header must not get wrong. Each of them is a way
 * of presenting a hub's claims as a user's words:
 *
 *  - the index is an assertion and never evidence. Everything in a BoardEntry is
 *    one hub's claim about one board: it can index a post that does not exist,
 *    attribute one user's words to another, change a subject line, send an entry
 *    to one subscriber and not another, or order a board so that a reply
 *    precedes what it replies to. Entries are scoped to the hub that sent them,
 *    and two hubs' entries for the same TTH describe a board that exists
 *    nowhere.
 *  - the fields a BoardEntry copies out of the document - parent and subject -
 *    are hints until the document has been fetched and hashed. A subject line
 *    shown before then is the hub's subject line, and nothing on screen may
 *    imply the post has been read.
 *  - Post::cid is never a proven identity. A CID is the hash of a secret and not
 *    a public key, so nothing can be signed with it: Post::cid is what the
 *    author typed into the document, and BoardEntry::cid is what one hub says it
 *    accepted the post from. Where the two disagree the honest case - the same
 *    document indexed on a second hub by somebody who did not write it - is
 *    indistinguishable from a forgery.
 *  - BoardEntry::nick is weaker still. It is not in the document at all, is
 *    unique on no hub and at no time, and is never used to attribute a post or
 *    to match an ignore list. Both of those belong to cid.
 *  - BoardEntry::size is the author's own claim, copied by the hub, and bounds
 *    nothing. A reader caps a transfer itself and abandons it when the cap is
 *    passed.
 *  - Board::name is protocol text to display and never a filename or a path
 *    component. The character set permits '.' and excludes the separators, so
 *    '..' is a legal board name; a cache on disk is named after the TTH. See
 *    ADC::isValidBoardName().
 *
 * Plain values, copied freely, owned by whoever holds them.
 */

#include "quickdc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace QuickDC {

/**
 * What a session may do on a board, as the PE field of BBD adds them up.
 *
 * Stated per session, because "what may *you* do here" is a question only the
 * hub can answer, and re-sent whenever the answer changes. It says what to offer
 * the user and is not a security boundary: the hub re-checks every operation as
 * it arrives, so one that PE appeared to allow may still be refused.
 *
 * Starting a thread is separate from replying to one because a board where
 * anyone may reply and only operators may post is the natural shape of an
 * announcements board, and it is also what a hub under load falls back to.
 */
enum BoardPermission {
	Subscribe   = 1,   /**<< Subscribe to the board and receive its entries. */
	StartThread = 2,   /**<< Post a new thread. */
	Reply       = 4,   /**<< Reply to an existing post. */
	WithdrawOwn = 8,   /**<< Withdraw a post of one's own. */
	WithdrawAny = 16   /**<< Withdraw any post on the board. */
};

/**
 * A board, as the hub describes it in BBD.
 *
 * A hub sends one of these per board a session may subscribe to, on entry to the
 * NORMAL state and again whenever any field of it changes. Withholding the
 * descriptor is the whole of the mechanism for hiding a board, so a board that
 * was never described is one this session cannot see rather than one that does
 * not exist.
 */
struct Board {
	/** BD: the board's name on this hub, and what BBL and BBP name it by. */
	std::string name;

	/** NI: a title for a person to read. Where empty, display 'name'. */
	std::string title;

	/** DE: a description for a person to read. Remote text; see the header. */
	std::string description;

	/** PE: what this session may do here. Ask it with may(). */
	uint64_t permissions = 0;

	/** MS: the largest post document, in bytes, the hub will index here. */
	uint64_t maxSize = 0;

	/** TS: the timestamp of the newest entry, or 0 when there is none. */
	uint64_t newest = 0;

	/**
	 * OT: the oldest timestamp the hub will replay.
	 *
	 * A stored cursor older than this cannot be caught up: what the client holds
	 * has a gap in it that no subscription will close, and it is a gap rather
	 * than a complete view of the board.
	 */
	uint64_t oldest = 0;

	/** NP: how many posts are indexed, tombstones excluded. */
	uint64_t posts = 0;

	/** RM1: the board is gone, and every subscription on it is cancelled. */
	bool removed = false;

	bool may(BoardPermission permission) const {
		return (permissions & static_cast<uint64_t>(permission)) != 0;
	}
};

/**
 * One post's entry in a hub's index, as BBL delivers it.
 *
 * Enough to build a threaded view, sort a board and decide what to fetch without
 * fetching anything - and no more than that, because none of it is evidence. An
 * entry is immutable except for withdrawal, which arrives as a second entry for
 * the same 'tth' carrying a later 'timestamp' and 'removed': a tombstone carries
 * only tth, board, timestamp and that flag, since the subject and author of a
 * withdrawn post are usually the reason it was withdrawn.
 */
struct BoardEntry {
	/** TR: the tiger tree hash of the post document. This is the post's identity. */
	std::string tth;

	/** SI: the document's length in bytes, as its author declared it. */
	uint64_t size = 0;

	/** BD: the board this entry is on, on the hub that sent it. */
	std::string board;

	/** ID: the CID of the session the hub says it accepted the post from. */
	std::string cid;

	/** NI: the nickname that session held at the time. Informative. */
	std::string nick;

	/** PA: copied from the document. Empty in a post that starts a thread. */
	std::string parent;

	/**
	 * TH: the thread root, as the hub grouped it.
	 *
	 * A convenience and never verified. A hub that groups threads wrongly
	 * produces a wrong display and nothing worse, because 'parent' is the
	 * load-bearing link and that one is inside the hash.
	 */
	std::string thread;

	/** SJ: copied from the document, and a hint until the document is held. */
	std::string subject;

	/**
	 * TS: when the hub accepted the post, in seconds since the epoch.
	 *
	 * Both the entry's place in the board's order and the cursor a subscription
	 * resumes from, which is why it is never corrected: the value is what the
	 * cursor is made of. An implausibly future timestamp is displayed as an
	 * unknown time rather than as a time.
	 */
	uint64_t timestamp = 0;

	/** RM1: the post has been withdrawn and this entry is its tombstone. */
	bool removed = false;
};

/**
 * A post document, parsed: its header line and its body.
 *
 * The document is the part of a post a hub cannot be trusted with, and the field
 * list is exactly that selection - the author, the parent, the subject and the
 * content. It carries no board and no thread root, so the same bytes are the
 * same post on every hub that indexes them.
 *
 * Composing one is: fill this in, ADC::writePostHeader() it, append the newline,
 * append the body, hash the result. The hash is not known until the final byte
 * is written, which is why a post cannot name itself and why a reply names its
 * parent rather than the reverse.
 */
struct Post {
	/** One header parameter this version of the format does not recognise. */
	struct Parameter {
		/** Two characters, as ADC names a parameter. */
		std::string name;
		std::string value;
	};

	/** ID: the CID the author claims. Required; never a proven identity. */
	std::string cid;

	/** PA: the TTH of the post being replied to, empty when it starts a thread. */
	std::string parent;

	/**
	 * SJ: the subject.
	 *
	 * Never contains a newline - a document whose SJ does is refused - and is
	 * remote text that reaches a dense list a user scans rather than reads, so
	 * whatever displays it strips control characters and bounds its length.
	 */
	std::string subject;

	/**
	 * DA: when the author says the post was composed, in seconds since the epoch.
	 *
	 * Nothing verifies it and a board is never ordered by it; BoardEntry::timestamp
	 * is the order. It exists because it is the only time that survives a post
	 * being carried to another hub, and it is presented as a claim rather than as
	 * a time.
	 */
	std::optional<uint64_t> date;

	/**
	 * RT, when the header carried it: 1 means the body is RTF0 rich text and
	 * every other value is reserved.
	 *
	 * The value and not a flag, because a value this version does not recognise
	 * has to survive being read and written back unchanged - see 'unrecognised'
	 * - and because a reserved value means plain text rather than a refusal.
	 * isRichText() is the question a renderer asks.
	 */
	std::optional<uint64_t> richTextValue;

	/** Everything after the header's newline, byte for byte, unescaped and raw. */
	std::string body;

	/**
	 * The header parameters this version does not recognise, in the order they
	 * were written, name and value each as they were.
	 *
	 * Not optional, and easy to leave out. Canonical form puts unrecognised
	 * parameters last, a reader must ignore them rather than reject a document
	 * carrying them, and canonicality is checked by re-serialising the header and
	 * comparing the bytes - so a Post that could not carry them would fail to
	 * reproduce any document that has them, and every such post would be refused
	 * as malformed. The names SG and KY are reserved for a future authorship
	 * extension and arrive here like any other: kept, never interpreted.
	 */
	std::vector<Parameter> unrecognised;

	/** Whether the body is RTF0 rich text: RT1 exactly, and nothing else. */
	bool isRichText() const { return richTextValue == 1; }
};

}

#endif // HAVE_QUICKDC_BULLETINS_API_H
