/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_BOARDKEEPER_H
#define HAVE_QUICKDC_BOARDKEEPER_H

#include "quickdc.h"

#include "api/attachments.h"
#include "api/bulletins.h"
#include "api/protocol.h"

#include <samurai/timer.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace QuickDC {

class BoardIndex;
class Core;
class PostStore;

/**
 * What this client holds of the bulletin boards its hubs describe, and every
 * decision about what it does on the network for them.
 *
 * A hub's boards are kept under the hub's address in an index on disk, so a board
 * can be read before its hub is connected and a subscription resumes where the
 * last one stopped. Post documents are kept by hash alone, shared by every hub
 * that indexes them, since the same bytes are the same post wherever they are.
 *
 *   following   every board a hub describes and lets this session subscribe to,
 *               from the highest timestamp held, unless somebody left it on this
 *               connection. A board the hub says is gone - a descriptor with RM1,
 *               or a refusal naming it as not there - is said to be gone.
 *   fetching    never because an entry arrived: a body is fetched from whoever
 *               holds it, at first only its author, so a fetch tells them who read
 *               it. A body is fetched when somebody asks to read it, and the rest
 *               of its thread with it only where "Fetch a thread ahead" holds for
 *               the hub. A document already here is read off the disk, which tells
 *               nobody anything. Each fetch is bounded by the size its entry
 *               declared and by FETCH_CAP, at most CONCURRENT_FETCHES go at once,
 *               a board may move ALLOWANCE_BYTES an hour, the author is named as a
 *               source only once a search has had AUTHOR_GRACE_MS to find somebody
 *               else, and a fetch nothing arrives for in FETCH_TIMEOUT_MS is given
 *               up on with the entry kept.
 *   offering    a verified document to anybody asking by its hash, where "Offer
 *               what I have read" holds for the hub it was read from - by default
 *               only on a hub kept as a favourite. Turning it off withholds.
 *   letting go  never as a side effect: a tombstone, the keep days and the cache
 *               size mark documents, and a marked one is deleted only once no
 *               upload is reading it.
 *
 * Core thread only. Nothing is made on disk until a board is first needed.
 */
class BoardKeeper : private Share::AttachmentListener, private Samurai::TimerListener {
	public:
		static constexpr uint64_t FETCH_CAP = 4u * 1024 * 1024;
		static constexpr size_t CONCURRENT_FETCHES = 2;
		static constexpr uint64_t ALLOWANCE_BYTES = 64u * 1024 * 1024;
		static constexpr uint64_t ALLOWANCE_MS = 3600u * 1000;
		static constexpr uint64_t AUTHOR_GRACE_MS = 20000;
		static constexpr uint64_t FETCH_TIMEOUT_MS = 300000;
		static constexpr int WATCH_MS = 2000;

		/* A replay from the start is a board's whole index in one burst; it is told in batches. */
		static constexpr size_t BATCH_ENTRIES = 256;
		static constexpr int BATCH_MS = 50;

		/** What it asks of one hub session. */
		class Link {
			public:
				virtual ~Link() { }
				virtual bool subscribeBoard(const std::string& board, uint64_t cursor) = 0;
				virtual bool unsubscribeBoard(const std::string& board) = 0;
				virtual bool submitPost(const std::string& tth, uint64_t size, const std::string& board,
				                        const std::string& subject, const std::string& parent) = 0;
				virtual bool requestEntry(const std::string& board, const std::string& tth) = 0;
				virtual bool withdrawPost(const std::string& board, const std::string& tth) = 0;
		};

		/** Steady milliseconds for the fetches, and seconds since the epoch for the store. */
		struct Clock {
			std::function<uint64_t()> steadyMs;
			std::function<uint64_t()> epochSeconds;
		};

		/**
		 * @param home where "boards", "posts", "incoming" and "outbox" are kept;
		 *        empty for a keeper that holds nothing.
		 * @param kept whether a hub, by its address, is one kept as a favourite.
		 */
		BoardKeeper(Core& core, std::string home, std::function<bool(const std::string&)> kept,
		            std::function<void(const Api::Event&)> publish);
		~BoardKeeper() override;

		BoardKeeper(const BoardKeeper&) = delete;
		BoardKeeper& operator=(const BoardKeeper&) = delete;

		void setClock(Clock clock);

		/* What a session hears. */
		void opened(Api::HubHandle hub, Link* link, const std::string& url);
		void closed(Api::HubHandle hub);
		void available(Api::HubHandle hub, bool available);
		void described(Api::HubHandle hub, const Board& board);
		void arrived(Api::HubHandle hub, const BoardEntry& entry);
		void refused(Api::HubHandle hub, int code, const std::string& board, const std::string& tth);

		/* What a front end asks. */
		void load(const std::string& hubUrl, const std::string& board);
		void subscribe(const std::string& hubUrl, const std::string& board, bool fromStart);
		void unsubscribe(const std::string& hubUrl, const std::string& board);
		void requestEntry(const std::string& hubUrl, const std::string& board, const std::string& tth);
		void withdraw(const std::string& hubUrl, const std::string& board, const std::string& tth);
		void read(const std::string& hubUrl, const std::string& board, const std::string& tth, bool fetch);
		void markRead(const std::string& hubUrl, const std::string& board, const std::string& tth);
		void write(const Api::Commands::WritePost& command);

		/** Whether 'filename' was a fetch of a post; it has been taken in where it was. */
		bool downloaded(const std::string& filename, const std::string& tth);

		/** [Boards], a hub's own choices or the favourites changed. */
		void policyChanged();

		/** An upload ended, so a document it was reading may be let go of now. */
		void transfersChanged();

		/** What the watch does every WATCH_MS; public so a test can be the clock. */
		void tick();

		/** Tell the entries that arrived since the last batch; public for the same reason. */
		void flush();

		/** Whether the hub at 'hubUrl' fetches a thread ahead, and offers what was read from it. */
		bool fetchesAhead(const std::string& hubUrl) const;
		bool serves(const std::string& hubUrl) const;

		/** The thread 'tth' is in on that board, as its parents say, without 'tth' and tombstones. */
		std::vector<BoardEntry> threadOf(const std::string& hubKey, const std::string& board,
		                                 const std::string& tth);

		/** Whether 'filename' is where a post is being fetched to, which nobody chose as a download. */
		bool isFetch(const std::string& filename) const;

		/** Where the documents are; empty for a keeper with no home. */
		std::string documentsDirectory() const;

	private:
		using State = Api::Events::PostStatus::State;
		using Why = Api::Events::PostStatus::Why;

		struct Session {
			Link* link = nullptr;
			std::string key;
			bool available = false;
			std::map<std::string, Board> described;
			std::set<std::string> subscribed;
			std::set<std::string> left;
		};

		struct Fetch {
			std::string hub;
			BoardEntry entry;
			std::string path;
			uint64_t cap = 0;
			/** When bytes last arrived, or the fetch started; both clocks run from it. */
			uint64_t since = 0;
			uint64_t moved = 0;
			bool authorNamed = false;
		};

		struct Spent {
			uint64_t bytes = 0;
			uint64_t since = 0;
		};

		struct Writing {
			Api::Commands::WritePost command;
			std::string hub;
			std::string subject;
			std::string path;
		};

		struct Store;

		Store* store();
		Session* session(Api::HubHandle hub);
		Session* sessionFor(const std::string& hubKey);

		/* The session on that address that carries boards, or none, which is told. */
		Session* carrying(const std::string& hubUrl, const std::string& board, const std::string& tth,
		                  Api::Events::BoardUnavailable::Asked asked);

		/** Subscribe from 'cursor' and say so; false where the session would not send it. */
		bool follow(Session& at, const std::string& board, uint64_t cursor);
		void announceState(const std::string& hubKey, const std::string& board, bool subscribed, bool gone);

		/** Offer what is held of a board's posts where its hub says so. */
		void offerBoard(const std::string& hubKey, const std::string& board);

		void fetch(const std::string& hubKey, const BoardEntry& entry);
		void start(const std::string& hubKey, const BoardEntry& entry);
		void startWaiting();
		void giveUp(const std::string& tth, State state, Why why, uint64_t size = 0);

		/** Decide what the document at 'path' is, take it in if it is the post, and say so. */
		void verify(const std::string& hubKey, const BoardEntry& entry, const std::string& path);

		bool withinAllowance(const std::string& hubKey, const std::string& board, uint64_t bytes);

		void tell(const std::string& tth, State state, Why why, uint64_t size = 0);

		/** Let go of the documents marked to go that no upload is reading. */
		void release();

		void evict();
		void watch();

		void EventAttachmentReady(const char* path, const char* tth, uint64_t size) override;
		void EventAttachmentFailed(const char* path, const char* reason) override;
		void EventTimeout(Samurai::Timer* timer) override;

		Core& core;
		std::string home;
		std::function<bool(const std::string&)> isKept;
		std::function<void(const Api::Event&)> publish;
		Clock clock;

		std::unique_ptr<Store> held;
		std::map<Api::HubHandle, Session> sessions;

		std::map<std::string, Fetch> fetching;
		std::deque<std::pair<std::string, BoardEntry>> waiting;
		std::map<std::string, Spent> allowances;
		std::map<std::string, State> states;

		/** The hub each document was read from, whose choice decides whether it is offered. */
		std::map<std::string, std::string> fetchedFrom;

		/** Whether each hub's documents are offered, as last applied. */
		std::map<std::string, bool> offering;

		/** Posts written here waiting to be hashed, by the file they were written to. */
		std::map<std::string, Writing> writing;

		/** Posts written here and submitted, by hash, until their entry comes back. */
		std::map<std::string, std::string> written;

		/** The view each board was last told as having, by hub and board. */
		std::map<std::pair<std::string, std::string>, Api::BoardView> views;

		/** What arrived and was kept and has not been told yet, by hub and board. */
		std::map<std::pair<std::string, std::string>, std::vector<BoardEntry>> arriving;
		size_t arrivingCount = 0;

		Samurai::Timer* timer = nullptr;
		Samurai::Timer* batch = nullptr;
};

}

#endif // HAVE_QUICKDC_BOARDKEEPER_H
