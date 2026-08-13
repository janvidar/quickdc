/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_LOCALHUB_BOARDS_H
#define HAVE_QUICKDC_ADC_LOCALHUB_BOARDS_H

#include "quickdc.h"

#include "api/bulletins.h"
#include "network/adc/server/user.h"

#include <chrono>
#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace QuickDC {
class Preferences;
}

namespace ADC {
namespace LocalHub {

/**
 * The most boards one hub will serve.
 *
 * Every board a session may subscribe to costs it one descriptor as it logs in,
 * so this is the size of a burst nobody asked for; and it bounds the work every
 * accepted post does, since a post is announced against the board it landed on
 * and the board has to be found first.
 */
inline constexpr size_t MAX_BOARDS = 64;

/**
 * How many boards one session may follow at once.
 *
 * A subscription is a standing instruction to send data, obtained with one short
 * command and available on every board at the same time. Without a ceiling one
 * session decides how much the hub sends it, which is the whole of the reason
 * this is bounded and not merely tidy.
 */
inline constexpr size_t MAX_SUBSCRIPTIONS_PER_SESSION = 16;

/**
 * What a board is where its configuration does not say.
 *
 * The size is what a client meets as MS, and 256k is a long post and a short
 * file. The two entry counts are deliberately small: the index is an unbounded
 * structure that remote users write to, a replay is sent in one pass, and a
 * board holding more than a session's send queue can carry is a board that
 * nobody on a slow link can ever catch up on.
 */
inline constexpr uint64_t BOARD_DEFAULT_MAX_SIZE = 256 * 1024;
inline constexpr uint64_t BOARD_DEFAULT_MAX_ENTRIES = 512;
inline constexpr uint64_t BOARD_DEFAULT_MAX_ENTRIES_PER_AUTHOR = 64;

/** Every permission BBS0 defines, added up: the most a mask may say. */
inline constexpr uint64_t BOARD_PERMISSIONS_ALL =
	static_cast<uint64_t>(QuickDC::Subscribe)
	| static_cast<uint64_t>(QuickDC::StartThread)
	| static_cast<uint64_t>(QuickDC::Reply)
	| static_cast<uint64_t>(QuickDC::WithdrawOwn)
	| static_cast<uint64_t>(QuickDC::WithdrawAny);

/**
 * What each credential may do on a board nobody has configured.
 *
 * A session that has authenticated as nothing at all may read, post, reply and
 * withdraw what it wrote, and may not withdraw anybody else's; an operator may
 * do the lot. Withholding permission 1 is how a board is hidden, so a default of
 * zero would make every board on a freshly configured hub invisible - which is a
 * hub that appears to have no boards rather than one that is locked down.
 */
inline constexpr uint64_t BOARD_DEFAULT_USER_PERMISSIONS =
	static_cast<uint64_t>(QuickDC::Subscribe)
	| static_cast<uint64_t>(QuickDC::StartThread)
	| static_cast<uint64_t>(QuickDC::Reply)
	| static_cast<uint64_t>(QuickDC::WithdrawOwn);

inline constexpr uint64_t BOARD_DEFAULT_OPERATOR_PERMISSIONS = BOARD_PERMISSIONS_ALL;

/**
 * The longest subject the hub will copy into an index entry.
 *
 * SJ on an entry is a copy of a document field, offered so that a client can
 * show a board without fetching it - a hint, and one the document overrides as
 * soon as it arrives. It is also remote text that the hub keeps on disk and
 * repeats to every subscriber for as long as the board lasts, so the copy is
 * bounded even though the document's own subject is not.
 */
inline constexpr size_t MAX_BOARD_SUBJECT = 256;

/**
 * The most of one board's index file the hub will read.
 *
 * A board holds BOARD_DEFAULT_MAX_ENTRIES entries of a few hundred bytes, so
 * this is far past any of them; it is here because the file is read at startup
 * and its size is decided by whatever happened to be on disk. Past the bound the
 * tail is read rather than the head, since the newest entries are the ones a
 * subscriber wants and the oldest are the ones the board would drop anyway.
 */
inline constexpr size_t MAX_INDEX_FILE = 4 * 1024 * 1024;

/**
 * What one session may spend on bulletins, per second and at once.
 *
 * Four allowances rather than one, because the four are not the same cost and a
 * client doing one of them properly must not be stopped by another. Requests are
 * commands, and the burst covers a client taking up as many subscriptions at once
 * as it is allowed to hold, with a few questions to spare - the opening move of a
 * client that already knows the hub. Replays are counted in entries, which is what
 * makes a resumption cheap and a replay from TS0 on a long board expensive - the
 * extension asks for exactly that distinction and this is the whole of it.
 *
 * Posting is slow and thread creation slower: an existing conversation is
 * bounded by the interest of the people in it, and starting one is not, so a
 * thread costs a token from both buckets and the stricter of the two decides.
 */
inline constexpr double BOARD_REQUEST_RATE = 1.0;
inline constexpr double BOARD_REQUEST_BURST = 24.0;
inline constexpr double BOARD_REPLAY_RATE = 200.0;
inline constexpr double BOARD_REPLAY_BURST = 4000.0;
inline constexpr double BOARD_POST_RATE = 0.1;
inline constexpr double BOARD_POST_BURST = 5.0;
inline constexpr double BOARD_THREAD_RATE = 0.02;
inline constexpr double BOARD_THREAD_BURST = 3.0;

/**
 * A token bucket, filled at a rate and spent by what a session asks for.
 *
 * The same shape as the inbound command allowance in User, and separate from it
 * for the same reason the four buckets here are separate from each other: what
 * exhausting it means differs. Running out of commands means the connection goes,
 * while running out of any of these means one refusal that names how long to
 * wait, since none of them is a reason to disconnect a client.
 *
 * Filled lazily, so an allowance costs nothing while it is not being spent, and
 * primed full: a session's first post arrives long before a rate of one per ten
 * seconds could have earned it a token.
 */
class Allowance {
	public:
		Allowance(double rate, double burst);

		/**
		 * Take 'cost' out of the bucket.
		 *
		 * @return false when there is not that much in it, having spent nothing -
		 *         a refused request must not also empty what it was refused from,
		 *         or a client retrying politely would never be allowed again.
		 */
		bool spend(double cost = 1.0);

		/**
		 * Whether 'cost' could be spent, without spending it.
		 *
		 * For an operation that spends from two buckets: a post that starts a
		 * thread pays into both, and taking from one before finding the other
		 * empty would charge a client for something it was refused.
		 */
		bool affords(double cost = 1.0) const;

		/**
		 * Seconds until 'cost' would be affordable, rounded up, and never zero.
		 *
		 * What goes in the TL field of a refusal. Rounded up and floored at one
		 * because it is an instruction to wait, and a client told to wait zero
		 * seconds retries at once into the same refusal.
		 */
		uint64_t retryAfter(double cost = 1.0) const;

	private:
		double rate;
		double burst;
		double tokens;
		std::chrono::steady_clock::time_point filled;
};

/**
 * What one session follows, and what it has left to spend on following it.
 *
 * Held per user and gone when the user is, which is one of the four ways a
 * subscription ends - the others being the client cancelling it, the session
 * losing permission 1 on the board, and the board ceasing to exist. Nothing else
 * cancels one: a client has no way to learn that a subscription has lapsed
 * except by noticing that nothing arrives.
 *
 * A board appears here at most once. A second BBL naming a board already
 * followed replaces the first and replays from the newly requested timestamp
 * rather than adding a second stream, so the map is the enforcement of that as
 * well as the record of it.
 */
class Subscriptions {
	public:
		Subscriptions();

		bool has(const std::string& board) const;
		size_t count() const;

		/**
		 * Note that this session has been sent a board's descriptor, and ask
		 * whether it ever was.
		 *
		 * Kept because a board that ceases to exist is announced with RM1, and the
		 * sessions that need to hear it are exactly the ones that were told the
		 * board was there. Telling every session would name a board to sessions it
		 * was hidden from, which is the one thing hiding a board has to avoid, and
		 * telling only the subscribers would leave a board in the interface of
		 * everybody who had not got round to opening it.
		 *
		 * Sticky: a session that loses permission 1 stops being sent descriptors
		 * but does not forget the board it already has on screen, so it is still
		 * owed the RM1 if the board later goes.
		 */
		void noteDescribed(const std::string& board);
		bool wasDescribed(const std::string& board) const;

		/** Forget a board entirely: this session neither follows it nor holds it. */
		void forget(const std::string& board);

		/**
		 * Follow a board from 'cursor' onwards, replacing any subscription this
		 * session already held on it.
		 *
		 * @return false when the session already follows
		 *         MAX_SUBSCRIPTIONS_PER_SESSION other boards. Replacing an
		 *         existing subscription always succeeds, since it adds nothing.
		 */
		bool subscribe(const std::string& board, uint64_t cursor);

		/** Stop following a board. Not an error where it was not followed. */
		void cancel(const std::string& board);

		/** Stop following everything, for a session that is going away. */
		void clear();

		/** The boards this session follows, so a caller can walk them safely. */
		std::vector<std::string> boards() const;

		/**
		 * The highest timestamp this session has been sent on a board, and 0
		 * where it follows the board but has been sent nothing.
		 *
		 * A subscription is a board and a point in time; this is the time part,
		 * kept current as entries go out so that the state of one is a single
		 * number rather than a history.
		 */
		uint64_t cursor(const std::string& board) const;

		/** Move the cursor to an entry that has been queued for this session. */
		void advance(const std::string& board, uint64_t timestamp);

		/** BBL commands. */
		Allowance requests;

		/** Index entries replayed, counted one per entry. */
		Allowance replays;

		/** BBP commands that write to an index. */
		Allowance posts;

		/** And the stricter allowance a post starting a thread also spends. */
		Allowance threads;

	private:
		std::map<std::string, uint64_t> cursors;

		/* Bounded by MAX_BOARDS: only a name the configuration holds gets here. */
		std::set<std::string> described;
};

/**
 * The boards this hub has, and the index it keeps for each of them.
 *
 * The hub holds an index and no bytes: for each board an ordered list of entries
 * naming post documents, in the order it accepted them. That is what makes a
 * board a board - it is per hub, it is ordered, and it is where permission and
 * moderation live - and it is all the hub has, since the documents themselves
 * travel between clients over the search and transfer machinery ADC already has.
 *
 * Three rules make the timestamp on an entry usable both as the board's order
 * and as the cursor a client resumes from, and they are the reason this class
 * hands out timestamps rather than letting a caller stamp an entry:
 *
 *  - an entry is never given a timestamp below the highest already assigned on
 *    its board;
 *  - where the clock has moved backwards the highest already assigned is reused
 *    rather than anything lower;
 *  - nothing enters the index in the past. An entry restored from a file, or
 *    imported from anywhere at all, takes the time of its arrival.
 *
 * Together they mean a cursor cannot be outflanked: a client that has caught up
 * to a given second will never ask for anything earlier, so an entry stamped
 * before that would be delivered to nobody. Nothing is ever inserted behind a
 * cursor, which is why BBS0 needs no generation counter and why a withdrawal is
 * given a *new* timestamp instead of keeping the one it is withdrawing.
 *
 * Every board is bounded, per board and per author, because the index is an
 * unbounded structure that remote users write to. The board bound drops the
 * oldest entries and says so through OT, which is what OT is for; the author
 * bound refuses the post instead, because dropping somebody's oldest post to
 * make room for their newest would make posting a way to unpublish - and a
 * client holding an entry the hub has quietly forgotten was never told, while a
 * withdrawal is a tombstone that reaches it.
 *
 * One file per board under the configured directory, named after a hash of the
 * board name and never after the name itself. A board name is protocol text: the
 * character set excludes the separators but permits '.', so ".." is a legal
 * board name, and so is "...". The name is also validated, but the file name is
 * where the mistake would be unrecoverable, so it does not depend on the
 * validation having been right.
 *
 * A file that has been truncated, or filled with something else entirely, costs
 * the entries that cannot be read and nothing more: every line is one complete
 * ADC message read back with the reader a client reads a BBL with, and one that
 * does not parse is skipped rather than repaired.
 */
class BoardStore {
	public:
		/**
		 * @param directory where the indexes live, "~" resolved as everywhere else
		 *        in the core. Created if it is not there, and a store whose
		 *        directory could not be made is not usable - see isUsable(), which
		 *        is what stops the hub announcing a feature it cannot honour.
		 */
		explicit BoardStore(const std::string& directory);
		virtual ~BoardStore();

		BoardStore(const BoardStore&) = delete;
		BoardStore& operator=(const BoardStore&) = delete;

		/** Why a post was not indexed, where the store is the one that decided. */
		enum class Outcome {
			Accepted,

			/** No such board, or none by that name any more. */
			NoBoard,

			/** That hash is already indexed here, tombstones included. */
			Duplicate,

			/** The author holds as many entries on this board as it may. */
			AuthorLimit
		};

		/**
		 * Read the board definitions out of the hub's configuration.
		 *
		 * One section per board, named "Local hub board " and the board's name, so
		 * that the section a hub operator writes is the board and there is no
		 * second place to remember to list it in. Every key is optional:
		 *
		 *     [Local hub board general]
		 *     Title=General discussion
		 *     Description=Anything about this hub
		 *     Max post size=262144
		 *     Max entries=512
		 *     Max entries per author=64
		 *     User permissions=15
		 *     Operator permissions=31
		 *     Admin permissions=31
		 *     Registered only=false
		 *
		 * The three permission masks are the PE of the extension added up - 1 to
		 * subscribe, 2 to start a thread, 4 to reply, 8 to withdraw one's own
		 * posts, 16 to withdraw anybody's - stated per credential, because "what
		 * may *you* do here" is the only form the question comes in. A mask of 0
		 * hides the board from that credential completely: no descriptor is sent
		 * and the board answers as though it were not there, which is the whole of
		 * the mechanism for hiding one.
		 *
		 * "Registered only" hides the board from an unregistered session in the
		 * same way, and is the one case where the hub says so rather than
		 * pretending the board does not exist: an operator who sets it wants a
		 * would-be reader to know that registering is the way in.
		 *
		 * Read without writing, unlike the hub's own settings: asking after a key
		 * through the ordinary getters stores the default it answered with, which
		 * for a group named after a board would write a board into the file that
		 * nobody had configured - and the next pass would then read it back as one.
		 *
		 * Called again on a re-read. A board that is still configured keeps the
		 * index it has loaded; one that has gone is forgotten, and the caller is
		 * the one that has to tell its subscribers, which is why names() is worth
		 * taking before this is called.
		 */
		void readConfiguration(QuickDC::Preferences* config);

		/** Whether the directory could be made. False makes the whole store inert. */
		bool isUsable() const;

		/** How many boards are configured. */
		size_t count() const;

		/** Every board's name, in the order the configuration holds them: by name. */
		std::vector<std::string> names() const;

		/** Whether a board exists at all, whatever any session may do on it. */
		bool exists(const std::string& board) const;

		/**
		 * What a session holding these credentials may do on a board.
		 *
		 * 0 both for a board that does not exist and for one this credential may
		 * do nothing on, which is deliberate: the two are indistinguishable to
		 * whoever asked, and telling them apart is what a hidden board must not do.
		 */
		uint64_t permissionsFor(const std::string& board, Credentials credentials) const;

		/**
		 * Whether a board is configured for registered sessions only.
		 *
		 * Asked when a session may do nothing on a board, to decide whether it is
		 * told that the board is registered-only or that there is no such board.
		 */
		bool registeredOnly(const std::string& board) const;

		/**
		 * The descriptor for a board as a session holding these credentials must
		 * be told it, permissions included.
		 *
		 * @return false where there is no such board. A board the credential may
		 *         do nothing on is described with a mask of 0, and it is the
		 *         caller's business not to send that.
		 */
		bool describe(const std::string& board, Credentials credentials,
		              QuickDC::Board& out) const;

		/** The entry for a hash on a board, tombstones included, or 0. */
		const QuickDC::BoardEntry* findEntry(const std::string& board,
		                                     const std::string& tth) const;

		/**
		 * Every entry on a board from 'from' onwards, oldest first.
		 *
		 * 'from' is clamped up to what the board will replay, which the descriptor
		 * declares as OT: a hub that has dropped its oldest entries is entitled to
		 * say so, and a request from before that is a request from there.
		 *
		 * The final second is included rather than excluded, and a client resuming
		 * from the highest timestamp it holds is meant to receive it again.
		 * Timestamps are not unique, so a cursor advanced one second past what it
		 * has seen would step over everything else accepted in that second.
		 */
		std::vector<QuickDC::BoardEntry> since(const std::string& board,
		                                       uint64_t from) const;

		/**
		 * Index a post.
		 *
		 * 'post' carries what the author declared - the hash, the size, the parent
		 * and the subject - and the CID and nickname the hub attests to. Its
		 * board, thread, timestamp and removed flag are ignored: those are the
		 * hub's to say, and 'out' is where it says them.
		 *
		 * The thread is the parent's thread where the parent is indexed here, and
		 * the post's own hash otherwise. A reply whose parent is on another hub, or
		 * has been withdrawn, is a post that starts a thread as far as this board
		 * is concerned, there being no thread here to attach it to.
		 */
		Outcome add(const std::string& board, const QuickDC::BoardEntry& post,
		            QuickDC::BoardEntry& out);

		/**
		 * Withdraw a post, replacing its entry with a tombstone.
		 *
		 * The tombstone keeps the hash and takes a new timestamp, which is what
		 * makes withdrawal work at all: it puts the tombstone at the head of the
		 * stream, so it reaches a subscriber at once and an absent client as soon
		 * as it resumes, where the original timestamp would have put it behind
		 * every cursor that had already moved past it. Strictly later than the
		 * entry it replaces, even where both happened in the same second, since a
		 * client ignores an arriving entry for a hash it holds unless the timestamp
		 * has moved on.
		 *
		 * The entry itself is not kept. A tombstone carries the hash, the board,
		 * the timestamp and the flag and should carry nothing else - the subject
		 * and the author of a withdrawn post are usually the reason it was
		 * withdrawn - so the hub stops holding what it has stopped repeating.
		 *
		 * @return false where the board or the entry is not there, or where the
		 *         entry is already a tombstone.
		 */
		bool withdraw(const std::string& board, const std::string& tth,
		              QuickDC::BoardEntry& tombstone);

		/** Where a board's index lives. Public so that a test can look at it. */
		std::string pathFor(const std::string& board) const;

	protected:
		/**
		 * The hub's clock, in seconds since the epoch.
		 *
		 * The one place the wall clock is read, and virtual because a test cannot
		 * ask the machine to move it backwards - which is the case the three
		 * monotonicity rules exist for and therefore the one that has to be
		 * exercised.
		 */
		virtual uint64_t now() const;

	private:
		/** One board: what the configuration says it is, and what it holds. */
		struct BoardState {
			std::string name;
			std::string title;
			std::string description;

			uint64_t maxSize = BOARD_DEFAULT_MAX_SIZE;
			uint64_t maxEntries = BOARD_DEFAULT_MAX_ENTRIES;
			uint64_t maxEntriesPerAuthor = BOARD_DEFAULT_MAX_ENTRIES_PER_AUTHOR;

			uint64_t userPermissions = BOARD_DEFAULT_USER_PERMISSIONS;
			uint64_t operatorPermissions = BOARD_DEFAULT_OPERATOR_PERMISSIONS;
			uint64_t adminPermissions = BOARD_DEFAULT_OPERATOR_PERMISSIONS;

			bool registeredOnly = false;

			/** Ordered by timestamp, which append order already makes it. */
			std::vector<QuickDC::BoardEntry> entries;

			/** The highest timestamp handed out here, which nothing goes below. */
			uint64_t highest = 0;

			/**
			 * The oldest timestamp this board will replay, and 0 while it will
			 * replay the lot.
			 *
			 * Only a board that has dropped something has anything to declare: a
			 * board holding everything it was ever given would otherwise tell a
			 * client with an older cursor that it has a gap, when what it has is
			 * the whole board.
			 */
			uint64_t oldest = 0;
		};

		BoardState* find(const std::string& board);
		const BoardState* find(const std::string& board) const;

		/** Fill a board's entries from its index file, once. */
		void load(BoardState& board);

		/** Append one entry to a board's index file. */
		void append(const BoardState& board, const QuickDC::BoardEntry& entry);

		/** Write a board's whole index out again, replacing what is there. */
		void rewrite(const BoardState& board);

		/**
		 * Drop the oldest entries until the board is within its bound.
		 *
		 * @return whether anything was dropped, which is when OT moved and when
		 *         what is on disk is no longer what is held.
		 */
		bool trim(BoardState& board);

		std::string directory;
		bool usable;

		/**
		 * In name order, and searched linearly.
		 *
		 * A vector because the order is what the descriptors go out in and what a
		 * hub operator will expect to see, and because MAX_BOARDS is small enough
		 * that a map would buy nothing but a second thing to keep in step.
		 */
		std::vector<BoardState> boards;
};

/**
 * The line one index entry occupies in a board's file: the BBL that would carry
 * it, without its terminating newline.
 *
 * Shared with the hub's sending path rather than a format of its own, so that
 * what is written down is what goes on the wire and is read back with
 * ADC::readEntry(). A tombstone is written the way it is sent: the hash, the
 * board, the timestamp and the flag, and nothing else.
 */
ADC::Command* writeEntry(const QuickDC::BoardEntry& entry);

/** And the descriptor for a board, as BBD carries it. */
ADC::Command* writeBoard(const QuickDC::Board& board);

} // namespace LocalHub
} // namespace ADC

#endif // HAVE_QUICKDC_ADC_LOCALHUB_BOARDS_H
