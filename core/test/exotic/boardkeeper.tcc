/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * What the core decides about bulletin boards: which boards it follows and from
 * where, when a post's document is fetched and when not, what a document that
 * arrives turns out to be, how long a fetch nobody answers is waited for, what
 * is offered to others and what is let go of.
 *
 * The keeper is driven the way its sessions drive it, and a hub session is
 * stood in for by a link that writes down what it was asked to say. The clock
 * is the case's, so the timeouts and the keep days are tested without waiting.
 * What goes over the wire is the qdc scenario's to test (cli/tests/boards.qdc).
 */

#include "quickdc.h"
#include "api/bulletins.h"
#include "api/core.h"
#include "api/huburl.h"
#include "api/protocol.h"
#include "bbs/boardkeeper.h"
#include "config/preferences.h"
#include "config/settings.h"
#include "network/adc/adcbbs.h"
#include "network/adc/cid.h"
#include "network/downloadqueue.h"

#include <samurai/crypto/digest/merkletree.h>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/file.h>

#include <samurai/posix.h>

#include <ctime>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

static QuickDC::Core* core = nullptr;
static std::string coreFiles;

EXO_SETUP({
	coreFiles = "/tmp/quickdc_boardkeeper_" + std::to_string((int) getpid());
	std::filesystem::create_directories(coreFiles);
	core = new QuickDC::Core(coreFiles + "/config.conf", coreFiles + "/session.conf", coreFiles + "/queue.xml");
	return core->queue && core->config && core->session ? 1 : 0;
});

EXO_TEARDOWN({
	delete core;
	core = nullptr;
	std::error_code ignored;
	std::filesystem::remove_all(coreFiles, ignored);
	return 1;
});

namespace {

using QuickDC::Api::Event;
using QuickDC::Api::Events::BoardEntries;
using QuickDC::Api::Events::BoardRead;
using QuickDC::Api::Events::BoardState;
using QuickDC::Api::Events::BoardUnavailable;
using QuickDC::Api::Events::PostRefused;
using QuickDC::Api::Events::PostStatus;
using State = PostStatus::State;
using Why = PostStatus::Why;

const uint64_t NOW = 1769000000;
const std::string AUTHOR = "QL3C44WZ5B3OIFIRHKBE3BM6LVUZBY4QB5QR33A";
const std::string STRANGER = "U2DF7CNEKD5CCNCHVVQ7I7DRDELQX46ZYOPFZ6Y";

std::string tigerOf(const std::string& bytes)
{
	Samurai::Crypto::Digest::Tiger tiger;
	Samurai::Crypto::Digest::MerkleTree tree(
		&tiger, Samurai::Crypto::Digest::DEFAULT_MERKLE_BLOCK_SIZE, QUICKDC_TTH_TREE_LEVELS);
	tree.update(bytes.data(), bytes.size());

	char printed[128];
	tree.digest().getFormattedString(Samurai::Crypto::Digest::HashValue::Format::Base32, printed, sizeof(printed));
	return printed;
}

struct Document {
	std::string bytes;
	QuickDC::BoardEntry entry;
};

Document post(const std::string& body, uint64_t timestamp, const std::string& parent = std::string(),
              const std::string& cid = AUTHOR)
{
	QuickDC::Post written;
	written.cid = cid;
	written.parent = parent;
	written.subject = parent.empty() ? "A subject" : "";

	Document made;
	made.bytes = ADC::writePostHeader(written) + "\n" + body;
	made.entry.tth = tigerOf(made.bytes);
	made.entry.size = made.bytes.size();
	made.entry.board = "general";
	made.entry.cid = AUTHOR;
	made.entry.nick = "alice";
	made.entry.parent = parent;
	made.entry.subject = written.subject;
	made.entry.timestamp = timestamp;
	return made;
}

QuickDC::BoardEntry tombstone(const std::string& tth, uint64_t timestamp)
{
	QuickDC::BoardEntry gone;
	gone.tth = tth;
	gone.board = "general";
	gone.timestamp = timestamp;
	gone.removed = true;
	return gone;
}

QuickDC::Board general()
{
	QuickDC::Board board;
	board.name = "general";
	board.title = "General discussion";
	board.permissions = QuickDC::Subscribe | QuickDC::StartThread | QuickDC::Reply;
	board.maxSize = 4096;
	return board;
}

class Link : public QuickDC::BoardKeeper::Link {
	public:
		bool subscribeBoard(const std::string& board, uint64_t cursor) override
		{
			subscribed.emplace_back(board, cursor);
			return true;
		}

		bool unsubscribeBoard(const std::string& board) override
		{
			left.push_back(board);
			return true;
		}

		bool submitPost(const std::string& tth, uint64_t, const std::string&, const std::string&,
		                const std::string&) override
		{
			submitted.push_back(tth);
			return true;
		}

		bool requestEntry(const std::string& board, const std::string& tth) override
		{
			asked.push_back(board + "/" + tth);
			return true;
		}

		bool withdrawPost(const std::string& board, const std::string& tth) override
		{
			withdrawn.push_back(board + "/" + tth);
			return true;
		}

		std::vector<std::pair<std::string, uint64_t>> subscribed;
		std::vector<std::string> left;
		std::vector<std::string> submitted;
		std::vector<std::string> asked;
		std::vector<std::string> withdrawn;
};

/* One keeper over a home of its own, on a hub of its own, with the case's clock. */
class Case {
	public:
		Case()
		{
			static int counter = 0;
			const int n = counter++;
			home = coreFiles + "/case" + std::to_string(n);
			hub = "adc://127.0.0.1:" + std::to_string(20000 + n);
			std::filesystem::create_directories(home);
			build();
		}

		~Case()
		{
			keeper.reset();
			for (const std::string& path : queued)
				if (QuickDC::QueueFile* file = core->queue->getFileByName(path.c_str())) core->queue->remove(file);
		}

		void build()
		{
			keeper = std::make_unique<QuickDC::BoardKeeper>(*core, home,
				[this](const std::string&) { return favourite; },
				[this](const Event& event) { events.push_back(event); });
			keeper->setClock({[this] { return steady; }, [this] { return epoch; }});
			keeper->opened(1, &link, hub);
			keeper->available(1, true);
		}

		/* What it said of type T, from 'from' on. */
		template<typename T>
		std::vector<T> told(size_t from = 0) const
		{
			std::vector<T> found;
			for (size_t n = from; n < events.size(); ++n)
				if (const T* e = events[n].as<T>()) found.push_back(*e);
			return found;
		}

		std::vector<PostStatus> statusOf(const std::string& tth, size_t from = 0) const
		{
			std::vector<PostStatus> found;
			for (const PostStatus& status : told<PostStatus>(from))
				if (status.tth == tth) found.push_back(status);
			return found;
		}

		std::string incoming(const std::string& tth) const { return home + "/incoming/" + tth; }

		bool isQueued(const std::string& tth)
		{
			if (!core->queue->getFileByName(incoming(tth).c_str())) return false;
			queued.push_back(incoming(tth));
			return true;
		}

		/* The fetch of 'doc' arriving with these bytes, as the queue says it did. */
		void arrive(const Document& doc, const std::string& bytes)
		{
			std::ofstream(incoming(doc.entry.tth), std::ios::binary) << bytes;
			if (QuickDC::QueueFile* file = core->queue->getFileByName(incoming(doc.entry.tth).c_str()))
				core->queue->remove(file);
			keeper->downloaded(incoming(doc.entry.tth), doc.entry.tth);
		}

		void fetchAndArrive(const Document& doc)
		{
			keeper->read(hub, "general", doc.entry.tth, true);
			arrive(doc, doc.bytes);
		}

		std::string home;
		std::string hub;
		Link link;
		bool favourite = false;
		uint64_t steady = 1000000;
		uint64_t epoch = NOW;
		std::vector<Event> events;
		std::vector<std::string> queued;
		std::unique_ptr<QuickDC::BoardKeeper> keeper;
};

}

EXO_TEST(boardkeeper_a_described_board_is_followed_from_the_highest_timestamp_held,
{
	Case c;
	c.keeper->arrived(1, post("one", NOW - 20).entry);
	c.keeper->arrived(1, post("two", NOW - 10).entry);
	c.keeper->described(1, general());

	EXO_ASSERT(c.link.subscribed.size() == 1);
	EXO_ASSERT(c.link.subscribed[0].first == "general");
	EXO_ASSERT(c.link.subscribed[0].second == NOW - 10);

	const std::vector<BoardState> states = c.told<BoardState>();
	EXO_ASSERT(!states.empty() && states.back().subscribed && !states.back().gone);
	EXO_ASSERT(states.back().hubUrl == c.hub && states.back().cursor == NOW - 10);

	/* A descriptor comes again whenever a field of the board changes; that is no new subscription. */
	c.keeper->described(1, general());
	EXO_ASSERT(c.link.subscribed.size() == 1);
	return 1;
});

EXO_TEST(boardkeeper_a_replay_reaching_the_hubs_oldest_closes_the_gap,
{
	Case c;
	QuickDC::Board board = general();
	board.oldest = NOW;
	c.keeper->described(1, board);
	EXO_ASSERT(c.told<BoardState>().back().view == QuickDC::Api::BoardView::Gap);

	c.keeper->arrived(1, post("oldest", NOW).entry);
	c.keeper->flush();
	const std::vector<BoardState> states = c.told<BoardState>();
	EXO_ASSERT(states.back().view == QuickDC::Api::BoardView::Complete && states.back().subscribed);

	/* Said once: the next batch changes nothing about it. */
	c.keeper->arrived(1, post("newer", NOW + 1).entry);
	c.keeper->flush();
	EXO_ASSERT(c.told<BoardState>().size() == states.size());
	return 1;
});

EXO_TEST(boardkeeper_a_board_left_stays_left_until_the_session_is_new,
{
	Case c;
	c.keeper->described(1, general());
	c.keeper->unsubscribe(c.hub, "general");
	EXO_ASSERT(c.link.left.size() == 1);

	c.keeper->described(1, general());
	EXO_ASSERT(c.link.subscribed.size() == 1);

	c.keeper->available(1, false);
	c.keeper->available(1, true);
	EXO_ASSERT(c.link.subscribed.size() == 2);

	/* And asked for by somebody, it is followed again at once, from the start if they say so. */
	c.keeper->unsubscribe(c.hub, "general");
	c.keeper->subscribe(c.hub, "general", true);
	EXO_ASSERT(c.link.subscribed.size() == 3 && c.link.subscribed[2].second == 0);
	return 1;
});

/*
 * A board is named by its hub's address everywhere, however that is spelled: what
 * needs the hub goes to the session on it, and with none there is refused, saying so.
 */
EXO_TEST(boardkeeper_what_needs_the_hub_finds_its_session_by_address,
{
	Case c;
	c.keeper->described(1, general());
	const Document one = post("one", NOW);
	const Document two = post("two", NOW + 1);
	const std::string spelled = " ADC://127.0.0.1:" + c.hub.substr(c.hub.rfind(':') + 1) + "/";

	c.keeper->requestEntry(spelled, "general", one.entry.tth);
	c.keeper->withdraw(spelled, "general", two.entry.tth);
	c.keeper->unsubscribe(spelled, "general");
	c.keeper->subscribe(spelled, "general", true);
	EXO_ASSERT(c.link.asked.size() == 1 && c.link.asked[0] == "general/" + one.entry.tth);
	EXO_ASSERT(c.link.withdrawn.size() == 1 && c.link.withdrawn[0] == "general/" + two.entry.tth);
	EXO_ASSERT(c.link.left.size() == 1);
	EXO_ASSERT(c.link.subscribed.size() == 2 && c.link.subscribed[1].second == 0);
	EXO_ASSERT(c.told<BoardUnavailable>().empty());

	const std::string elsewhere = "adc://elsewhere.example:1511";
	c.keeper->subscribe(elsewhere, "general", false);
	c.keeper->unsubscribe(elsewhere, "general");
	c.keeper->requestEntry(elsewhere, "general", one.entry.tth);
	c.keeper->withdraw(elsewhere, "general", one.entry.tth);
	const std::vector<BoardUnavailable> refused = c.told<BoardUnavailable>();
	EXO_ASSERT(refused.size() == 4);
	EXO_ASSERT(refused[0].asked == BoardUnavailable::Asked::Subscribe && refused[0].hubUrl == elsewhere);
	EXO_ASSERT(refused[1].asked == BoardUnavailable::Asked::Unsubscribe);
	EXO_ASSERT(refused[2].asked == BoardUnavailable::Asked::Entry && refused[2].tth == one.entry.tth);
	EXO_ASSERT(refused[3].asked == BoardUnavailable::Asked::Withdraw && refused[3].board == "general");

	/* A session without boards is no session to ask either. */
	c.keeper->available(1, false);
	c.keeper->withdraw(c.hub, "general", one.entry.tth);
	EXO_ASSERT(c.told<BoardUnavailable>().size() == 5 && c.link.withdrawn.size() == 1);
	return 1;
});

EXO_TEST(boardkeeper_a_board_its_hub_has_not_got_is_gone,
{
	Case c;
	c.keeper->described(1, general());

	QuickDC::Board removed;
	removed.name = "general";
	removed.removed = true;
	c.keeper->described(1, removed);

	std::vector<BoardState> states = c.told<BoardState>();
	EXO_ASSERT(states.back().board == "general" && states.back().gone && !states.back().subscribed);

	c.keeper->refused(1, 71, "elsewhere", "");
	states = c.told<BoardState>();
	EXO_ASSERT(states.back().board == "elsewhere" && states.back().gone);

	/* A refusal for any other reason - a rate limit - leaves the board where it was. */
	const size_t before = c.events.size();
	c.keeper->refused(1, 75, "general", "");
	EXO_ASSERT(c.told<BoardState>(before).empty());
	return 1;
});

EXO_TEST(boardkeeper_an_entry_arriving_fetches_nothing,
{
	Case c;
	const Document doc = post("hello", NOW);
	c.keeper->arrived(1, doc.entry);
	c.keeper->flush();

	const std::vector<BoardEntries> kept = c.told<BoardEntries>();
	EXO_ASSERT(kept.size() == 1 && kept[0].entries.size() == 1 && kept[0].unread == 1 && !kept[0].whole);
	EXO_ASSERT(c.statusOf(doc.entry.tth).empty());
	EXO_ASSERT(!c.isQueued(doc.entry.tth));

	/* A replay of the same second is not kept twice. */
	c.keeper->arrived(1, doc.entry);
	c.keeper->flush();
	EXO_ASSERT(c.told<BoardEntries>().size() == 1);
	return 1;
});

EXO_TEST(boardkeeper_a_replay_is_told_in_batches_rather_than_an_event_an_entry,
{
	Case c;
	const size_t replayed = QuickDC::BoardKeeper::BATCH_ENTRIES + 10;
	for (size_t n = 0; n < replayed; ++n) c.keeper->arrived(1, post("entry " + std::to_string(n), NOW + n).entry);

	std::vector<BoardEntries> told = c.told<BoardEntries>();
	EXO_ASSERT(told.size() == 1 && told[0].entries.size() == QuickDC::BoardKeeper::BATCH_ENTRIES);

	c.keeper->flush();
	told = c.told<BoardEntries>();
	EXO_ASSERT(told.size() == 2 && told[1].entries.size() == 10 && told[1].unread == static_cast<int>(replayed));
	return 1;
});

EXO_TEST(boardkeeper_a_post_is_fetched_when_read_and_its_thread_only_where_the_hub_says,
{
	Case c;
	const Document root = post("root", NOW);
	const Document one = post("one", NOW + 1, root.entry.tth);
	const Document two = post("two", NOW + 2, root.entry.tth);
	const Document deeper = post("deeper", NOW + 3, one.entry.tth);
	for (const Document* doc : {&root, &one, &two, &deeper}) c.keeper->arrived(1, doc->entry);

	/* Opened without asking for the body, it is read and nothing is fetched. */
	c.keeper->read(c.hub, "general", root.entry.tth, false);
	EXO_ASSERT(c.told<BoardRead>().size() == 1 && c.told<BoardRead>()[0].unread == 3);
	EXO_ASSERT(!c.isQueued(root.entry.tth));

	c.keeper->read(c.hub, "general", one.entry.tth, true);
	EXO_ASSERT(c.isQueued(one.entry.tth));
	EXO_ASSERT(!c.isQueued(two.entry.tth) && !c.isQueued(deeper.entry.tth) && !c.isQueued(root.entry.tth));
	EXO_ASSERT(c.statusOf(one.entry.tth).back().state == State::Fetching);
	EXO_ASSERT(!c.keeper->fetchesAhead(c.hub));

	core->session->setBool(QuickDC::hubSettingsGroup(c.hub), QuickDC::Settings::BoardPrefetch.key, true);
	EXO_ASSERT(c.keeper->fetchesAhead(c.hub));
	c.arrive(one, one.bytes);

	/* The rest of the thread, two at a time; a grandchild is in it as much as a child. */
	c.keeper->read(c.hub, "general", deeper.entry.tth, true);
	EXO_ASSERT(c.isQueued(deeper.entry.tth));
	EXO_ASSERT(c.isQueued(root.entry.tth));
	EXO_ASSERT(!c.isQueued(two.entry.tth));
	c.arrive(root, root.bytes);
	EXO_ASSERT(c.isQueued(two.entry.tth));

	core->session->remove(QuickDC::hubSettingsGroup(c.hub), QuickDC::Settings::BoardPrefetch.key);
	return 1;
});

EXO_TEST(boardkeeper_a_document_is_verified_attributed_to_the_index_and_then_read_off_the_disk,
{
	Case c;
	const Document mine = post("mine", NOW);
	const Document theirs = post("theirs", NOW + 1, std::string(), STRANGER);
	c.keeper->arrived(1, mine.entry);
	c.keeper->arrived(1, theirs.entry);

	c.fetchAndArrive(mine);
	PostStatus told = c.statusOf(mine.entry.tth).back();
	EXO_ASSERT(told.state == State::Verified && told.post.body == "mine" && told.attributedTo == AUTHOR);
	EXO_ASSERT(Samurai::IO::File(c.home + "/posts/" + mine.entry.tth).exists());

	/* The document names somebody else: shown, and attributed to the hub's claim. */
	c.fetchAndArrive(theirs);
	told = c.statusOf(theirs.entry.tth).back();
	EXO_ASSERT(told.state == State::Disputed && told.post.cid == STRANGER && told.attributedTo == AUTHOR);

	/* Held, it is read without asking anybody, after a restart too. */
	c.keeper.reset();
	c.build();
	const size_t before = c.events.size();
	c.keeper->read(c.hub, "general", mine.entry.tth, false);
	EXO_ASSERT(c.statusOf(mine.entry.tth, before).back().state == State::Verified);
	EXO_ASSERT(!c.isQueued(mine.entry.tth));
	return 1;
});

EXO_TEST(boardkeeper_a_document_that_is_not_the_post_is_never_kept_as_one,
{
	Case c;
	const Document wrong = post("wrong", NOW);
	const Document garbage = post("garbage", NOW + 1);
	c.keeper->arrived(1, wrong.entry);

	/* Other bytes than the hash names: thrown away, and the post may be asked for again. */
	c.keeper->read(c.hub, "general", wrong.entry.tth, true);
	c.arrive(wrong, "IBB0 IDsomebody\nnot this");
	EXO_ASSERT(c.statusOf(wrong.entry.tth).back().state == State::Unavailable);
	EXO_ASSERT(c.statusOf(wrong.entry.tth).back().why == Why::WrongHash);
	EXO_ASSERT(!Samurai::IO::File(c.incoming(wrong.entry.tth)).exists());

	/* Bytes that hash to the name and are not a post: every copy is these, so it is known bad. */
	Document notAPost = garbage;
	notAPost.bytes = "this is no post document at all";
	notAPost.entry.tth = tigerOf(notAPost.bytes);
	notAPost.entry.size = notAPost.bytes.size();
	c.keeper->arrived(1, notAPost.entry);
	c.fetchAndArrive(notAPost);
	EXO_ASSERT(c.statusOf(notAPost.entry.tth).back().state == State::Unreadable);
	EXO_ASSERT(c.statusOf(notAPost.entry.tth).back().why == Why::NotAPost);

	const size_t before = c.events.size();
	c.keeper->read(c.hub, "general", notAPost.entry.tth, true);
	EXO_ASSERT(c.statusOf(notAPost.entry.tth, before).back().why == Why::KnownBad);
	EXO_ASSERT(!c.isQueued(notAPost.entry.tth));
	return 1;
});

EXO_TEST(boardkeeper_a_fetch_nobody_answers_names_the_author_and_then_gives_up,
{
	Case c;
	const Document doc = post("lonely", NOW);
	c.keeper->arrived(1, doc.entry);
	c.keeper->read(c.hub, "general", doc.entry.tth, true);

	QuickDC::QueueFile* file = core->queue->getFileByName(c.incoming(doc.entry.tth).c_str());
	EXO_ASSERT(file && file->countSources() == 0);

	c.steady += QuickDC::BoardKeeper::AUTHOR_GRACE_MS - 1;
	c.keeper->tick();
	EXO_ASSERT(file->countSources() == 0);

	c.steady += 1;
	c.keeper->tick();
	EXO_ASSERT(file->countSources() == 1);

	c.steady += QuickDC::BoardKeeper::FETCH_TIMEOUT_MS;
	c.keeper->tick();
	EXO_ASSERT(c.statusOf(doc.entry.tth).back().state == State::Unavailable);
	EXO_ASSERT(c.statusOf(doc.entry.tth).back().why == Why::NobodyAnswered);
	EXO_ASSERT(!core->queue->getFileByName(c.incoming(doc.entry.tth).c_str()));

	/* The entry stays, and asking again is a fetch again. */
	c.keeper->read(c.hub, "general", doc.entry.tth, true);
	EXO_ASSERT(c.isQueued(doc.entry.tth));
	return 1;
});

EXO_TEST(boardkeeper_fetches_are_bounded_by_size_by_number_and_by_board,
{
	Case c;
	Document large = post("large", NOW);
	large.entry.size = QuickDC::BoardKeeper::FETCH_CAP + 1;
	c.keeper->arrived(1, large.entry);
	c.keeper->read(c.hub, "general", large.entry.tth, true);
	EXO_ASSERT(c.statusOf(large.entry.tth).back().why == Why::TooLarge);
	EXO_ASSERT(!c.isQueued(large.entry.tth));

	const Document a = post("a", NOW + 1);
	const Document b = post("b", NOW + 2);
	const Document third = post("third", NOW + 3);
	for (const Document* doc : {&a, &b, &third}) c.keeper->arrived(1, doc->entry);
	for (const Document* doc : {&a, &b, &third}) c.keeper->read(c.hub, "general", doc->entry.tth, true);

	EXO_ASSERT(c.isQueued(a.entry.tth) && c.isQueued(b.entry.tth));
	EXO_ASSERT(!c.isQueued(third.entry.tth));
	c.arrive(a, a.bytes);
	EXO_ASSERT(c.isQueued(third.entry.tth));
	return 1;
});

EXO_TEST(boardkeeper_a_tombstone_withdraws_the_post_and_lets_its_document_go,
{
	Case c;
	const Document doc = post("regret", NOW);
	c.keeper->arrived(1, doc.entry);
	c.fetchAndArrive(doc);
	EXO_ASSERT(Samurai::IO::File(c.home + "/posts/" + doc.entry.tth).exists());

	c.keeper->arrived(1, tombstone(doc.entry.tth, NOW + 5));
	c.keeper->flush();
	EXO_ASSERT(c.statusOf(doc.entry.tth).back().state == State::Withdrawn);
	EXO_ASSERT(!Samurai::IO::File(c.home + "/posts/" + doc.entry.tth).exists());

	const std::vector<BoardEntries> kept = c.told<BoardEntries>();
	EXO_ASSERT(kept.back().entries.size() == 1 && kept.back().entries[0].removed);

	/* Asked for afterwards, it is withdrawn and nothing is fetched. */
	c.keeper->read(c.hub, "general", doc.entry.tth, true);
	EXO_ASSERT(!c.isQueued(doc.entry.tth));
	EXO_ASSERT(c.statusOf(doc.entry.tth).back().state == State::Withdrawn);
	return 1;
});

EXO_TEST(boardkeeper_documents_past_the_cache_size_or_the_keep_days_are_let_go,
{
	Case c;
	const Document old = post("old", NOW);
	const Document fresh = post("fresh", NOW + 1);
	c.keeper->arrived(1, old.entry);
	c.keeper->arrived(1, fresh.entry);
	c.fetchAndArrive(old);
	c.fetchAndArrive(fresh);

	/* The oldest taken in goes first, until what is left fits. */
	QuickDC::Settings::write(*core->config, QuickDC::Settings::BoardCache, static_cast<int>(fresh.bytes.size()));
	c.keeper->policyChanged();
	EXO_ASSERT(c.statusOf(old.entry.tth).back().why == Why::Evicted);
	EXO_ASSERT(c.statusOf(fresh.entry.tth).back().state == State::Verified);
	EXO_ASSERT(!Samurai::IO::File(c.home + "/posts/" + old.entry.tth).exists());
	EXO_ASSERT(Samurai::IO::File(c.home + "/posts/" + fresh.entry.tth).exists());

	QuickDC::Settings::write(*core->config, QuickDC::Settings::BoardKeepDays, 1);
	c.epoch = static_cast<uint64_t>(time(0)) + 2 * 24 * 60 * 60;
	c.keeper->policyChanged();
	EXO_ASSERT(c.statusOf(fresh.entry.tth).back().why == Why::Evicted);

	core->config->remove(QuickDC::Settings::BoardKeepDays.group, QuickDC::Settings::BoardKeepDays.key);
	core->config->remove(QuickDC::Settings::BoardCache.group, QuickDC::Settings::BoardCache.key);
	return 1;
});

EXO_TEST(boardkeeper_what_is_read_is_offered_only_where_the_hub_says,
{
	Case c;
	const std::string group = QuickDC::hubSettingsGroup(c.hub);

	/* A hub only visited is told nothing of what was read on it. */
	EXO_ASSERT(!c.keeper->serves(c.hub));

	c.favourite = true;
	EXO_ASSERT(c.keeper->serves(c.hub));

	core->session->setBool(group, QuickDC::Settings::BoardServe.key, false);
	EXO_ASSERT(!c.keeper->serves(c.hub));

	c.favourite = false;
	core->session->setBool(group, QuickDC::Settings::BoardServe.key, true);
	EXO_ASSERT(c.keeper->serves(c.hub));

	QuickDC::Settings::write(*core->config, QuickDC::Settings::BoardServe, false);
	EXO_ASSERT(c.keeper->serves(c.hub));
	core->session->remove(group, QuickDC::Settings::BoardServe.key);
	EXO_ASSERT(!c.keeper->serves(c.hub));

	core->config->remove(QuickDC::Settings::BoardServe.group, QuickDC::Settings::BoardServe.key);
	return 1;
});

EXO_TEST(boardkeeper_what_was_read_is_still_read_when_the_board_is_read_back,
{
	Case c;
	const Document one = post("one", NOW);
	const Document two = post("two", NOW + 1);
	c.keeper->arrived(1, one.entry);
	c.keeper->arrived(1, two.entry);
	c.keeper->markRead(c.hub, "general", one.entry.tth);

	c.keeper.reset();
	c.build();
	const size_t restarted = c.events.size();
	c.keeper->load(c.hub + "/", "general");

	const std::vector<BoardEntries> loaded = c.told<BoardEntries>(restarted);
	EXO_ASSERT(loaded.size() == 1 && loaded[0].whole && loaded[0].entries.size() == 2);
	EXO_ASSERT(loaded[0].unread == 1 && loaded[0].read.size() == 1 && loaded[0].read[0] == one.entry.tth);

	/* Read twice is read once, and said once. */
	const size_t before = c.events.size();
	c.keeper->markRead(c.hub, "general", one.entry.tth);
	EXO_ASSERT(c.told<BoardRead>(before).empty());

	c.keeper->markRead(c.hub, "general", std::string());
	const std::vector<BoardRead> all = c.told<BoardRead>(before);
	EXO_ASSERT(all.size() == 1 && all[0].whole && all[0].unread == 0 && all[0].read.size() == 2);
	return 1;
});

EXO_TEST(boardkeeper_a_post_that_cannot_be_written_says_why,
{
	Case c;
	c.keeper->described(1, general());

	QuickDC::Api::Commands::WritePost thread;
	thread.request = 7;
	thread.hubUrl = c.hub;
	thread.board = "general";
	thread.subject = "   \n  ";
	thread.body = "no subject";
	c.keeper->write(thread);
	EXO_ASSERT(c.told<PostRefused>().back().why == PostRefused::Why::NoSubject);

	thread.subject = "Long";
	thread.body = std::string(5000, 'x');
	c.keeper->write(thread);
	const PostRefused large = c.told<PostRefused>().back();
	EXO_ASSERT(large.why == PostRefused::Why::TooLarge && large.limit == 4096 && large.size > 5000);

	thread.hubUrl = "adc://elsewhere.example:1511";
	c.keeper->write(thread);
	EXO_ASSERT(c.told<PostRefused>().back().why == PostRefused::Why::NoHub);
	EXO_ASSERT(c.link.submitted.empty());
	return 1;
});

EXO_TEST(boardkeeper_a_written_post_is_the_document_its_hash_covers,
{
	Case c;
	c.keeper->described(1, general());

	QuickDC::Api::Commands::WritePost thread;
	thread.request = 8;
	thread.hubUrl = c.hub;
	thread.board = "general";
	thread.subject = "Hub upgrade\n on  Saturday";
	thread.body = "The hub will be **offline** on Saturday.\n\nSee you then.";
	thread.richText = true;
	c.keeper->write(thread);
	EXO_ASSERT(c.told<PostRefused>().empty());

	std::vector<std::filesystem::path> written;
	for (const auto& entry : std::filesystem::directory_iterator(c.home + "/outbox")) written.push_back(entry.path());
	EXO_ASSERT(written.size() == 1);

	std::ifstream in(written[0], std::ios::binary);
	const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const size_t newline = bytes.find('\n');
	EXO_ASSERT(newline != std::string::npos);

	/* One ADC message escaped as the grammar asks and no more, the subject on one line. */
	const std::string header = bytes.substr(0, newline);
	const std::string signedBy = std::string("IBB0 ID") + core->identity->getCID() + " SJHub\\supgrade\\son\\sSaturday DA";
	EXO_ASSERT(header.rfind(signedBy, 0) == 0);
	EXO_ASSERT(header.size() > 4 && header.compare(header.size() - 4, 4, " RT1") == 0);

	/* The body raw after the first newline, which is where ADC stops. */
	EXO_ASSERT(bytes.substr(newline + 1) == thread.body);

	QuickDC::Post parsed;
	EXO_ASSERT(ADC::parsePost(bytes.data(), bytes.size(), parsed));
	EXO_ASSERT(parsed.subject == "Hub upgrade on Saturday" && parsed.isRichText());
	return 1;
});
