/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "bbs/boardkeeper.h"

#include "api/core.h"
#include "api/huburl.h"
#include "bbs/boardindex.h"
#include "bbs/poststore.h"
#include "config/preferences.h"
#include "config/settings.h"
#include "hash/hashjob.h"
#include "network/adc/adcbbs.h"
#include "network/adc/adccommon.h"
#include "network/adc/cid.h"
#include "network/downloadqueue.h"
#include "network/transfer.h"
#include "network/transfermanager.h"
#include "share/sharemanager.h"

#include <samurai/crypto/digest/merkletree.h>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/file.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

struct QuickDC::BoardKeeper::Store {
	Store(const std::string& boards, const std::string& documents, Share::Manager* shares)
		: index(boards)
		, posts(documents, shares)
	{
	}

	BoardIndex index;
	PostStore posts;
};

namespace {

using QuickDC::Api::BoardView;

BoardView converted(QuickDC::BoardIndex::View view)
{
	switch (view)
	{
		case QuickDC::BoardIndex::View::Complete: return BoardView::Complete;
		case QuickDC::BoardIndex::View::Gap:      return BoardView::Gap;
		case QuickDC::BoardIndex::View::Unknown:  break;
	}
	return BoardView::Unknown;
}

std::string tigerTreeRoot(const std::string& document)
{
	Samurai::Crypto::Digest::Tiger tiger;
	Samurai::Crypto::Digest::MerkleTree tree(
		&tiger, Samurai::Crypto::Digest::DEFAULT_MERKLE_BLOCK_SIZE, QUICKDC_TTH_TREE_LEVELS);
	tree.update(document.data(), document.size());

	char printed[128];
	if (!tree.digest().getFormattedString(
			Samurai::Crypto::Digest::HashValue::Format::Base32, printed, sizeof(printed)))
		return std::string();

	return printed;
}

std::string leafOf(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

/* One line, its runs of white space one space: the subject is one ADC parameter. */
std::string oneLine(const std::string& text)
{
	std::string out;
	bool space = false;

	for (const char c : text)
	{
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f')
		{
			space = !out.empty();
			continue;
		}

		if (space) out += ' ';
		space = false;
		out += c;
	}

	return out;
}

uint64_t steadyNow()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string boardKey(const std::string& hubKey, const std::string& board)
{
	return hubKey + '\n' + board;
}

}

QuickDC::BoardKeeper::BoardKeeper(Core& core_, std::string home_, std::function<bool(const std::string&)> kept_,
                                  std::function<void(const Api::Event&)> publish_)
	: core(core_)
	, home(std::move(home_))
	, isKept(std::move(kept_))
	, publish(std::move(publish_))
	, clock{steadyNow, [] { return static_cast<uint64_t>(time(0)); }}
{
}

QuickDC::BoardKeeper::~BoardKeeper()
{
	delete timer;
	delete batch;

	/* A document still being hashed would otherwise report to this. */
	if (core.shares) core.shares->forgetListener(this);
}

void QuickDC::BoardKeeper::setClock(Clock replacement)
{
	clock = std::move(replacement);
}

std::string QuickDC::BoardKeeper::documentsDirectory() const
{
	return home.empty() ? std::string() : home + "/posts";
}

bool QuickDC::BoardKeeper::isFetch(const std::string& filename) const
{
	return !home.empty() && filename.rfind(home + "/incoming/", 0) == 0;
}

QuickDC::BoardKeeper::Store* QuickDC::BoardKeeper::store()
{
	if (held || home.empty()) return held.get();

	std::error_code ignored;
	std::filesystem::create_directories(home + "/incoming", ignored);

	held = std::make_unique<Store>(home + "/boards", home + "/posts", core.shares);

	/* An offer lasts no longer than the process that made it, so nothing can be reading these. */
	for (const std::string& tth : held->posts.pending()) held->posts.release(tth);

	evict();
	return held.get();
}

QuickDC::BoardKeeper::Session* QuickDC::BoardKeeper::session(Api::HubHandle hub)
{
	const auto found = sessions.find(hub);
	return found == sessions.end() ? nullptr : &found->second;
}

QuickDC::BoardKeeper::Session* QuickDC::BoardKeeper::sessionFor(const std::string& hubKey)
{
	for (auto& [handle, at] : sessions)
		if (at.key == hubKey) return &at;
	return nullptr;
}

bool QuickDC::BoardKeeper::fetchesAhead(const std::string& hubUrl) const
{
	const std::string key = hubSettingsGroup(hubUrl);
	const bool general = core.config && Settings::read(*core.config, Settings::BoardPrefetch);
	if (!core.session) return general;
	return core.session->peekBool(key, Settings::BoardPrefetch.key, general);
}

bool QuickDC::BoardKeeper::serves(const std::string& hubUrl) const
{
	const std::string key = hubSettingsGroup(hubUrl);
	if (core.session && core.session->haveSetting(key, Settings::BoardServe.key))
		return core.session->peekBool(key, Settings::BoardServe.key, false);

	/* Offering says "I have read this" to whoever asks, so a hub only visited says nothing. */
	if (!isKept || !isKept(key)) return false;
	return core.config && Settings::read(*core.config, Settings::BoardServe);
}

void QuickDC::BoardKeeper::opened(Api::HubHandle hub, Link* link, const std::string& url)
{
	Session& at = sessions[hub];
	at.link = link;
	at.key = hubSettingsGroup(url);
}

void QuickDC::BoardKeeper::closed(Api::HubHandle hub)
{
	sessions.erase(hub);
}

void QuickDC::BoardKeeper::available(Api::HubHandle hub, bool now)
{
	Session* at = session(hub);
	if (!at || at->available == now) return;

	at->available = now;
	at->subscribed.clear();
	at->left.clear();
	if (!now) return;

	/* A session that has just found the feature holds no subscription, so each board described is followed again. */
	if (!store()) return;
	for (const auto& [name, board] : at->described)
		if (board.may(BoardPermission::Subscribe))
			follow(*at, name, held->index.cursor(at->key, name));
}

bool QuickDC::BoardKeeper::follow(Session& at, const std::string& board, uint64_t cursor)
{
	if (!at.link || !at.link->subscribeBoard(board, cursor)) return false;

	at.subscribed.insert(board);
	at.left.erase(board);
	offerBoard(at.key, board);
	announceState(at.key, board, true, false);
	return true;
}

void QuickDC::BoardKeeper::announceState(const std::string& hubKey, const std::string& board,
                                         bool subscribed, bool gone)
{
	Api::Events::BoardState state;
	state.hubUrl = hubKey;
	state.board = board;
	state.subscribed = subscribed;
	state.gone = gone;

	if (Session* at = sessionFor(hubKey))
	{
		state.left = at->left.count(board) > 0;
		const auto known = at->described.find(board);
		state.described = known != at->described.end();
		if (state.described) state.descriptor = known->second;
	}
	if (gone) state.descriptor.name = board;
	if (gone) state.descriptor.removed = true;

	if (Store* here = store())
	{
		state.cursor = here->index.cursor(hubKey, board);
		state.view = converted(here->index.view(hubKey, board));
	}

	views[{hubKey, board}] = state.view;
	publish(state);
}

void QuickDC::BoardKeeper::described(Api::HubHandle hub, const Board& board)
{
	Session* at = session(hub);
	if (!at || board.name.empty()) return;

	if (board.removed)
	{
		/* Its entries stay readable; recording this would turn a view with a hole into a whole one. */
		at->described.erase(board.name);
		at->subscribed.erase(board.name);
		announceState(at->key, board.name, false, true);
		return;
	}

	at->described[board.name] = board;
	if (Store* here = store()) here->index.describe(at->key, board);

	/*
	 * Followed from the moment the hub says it is there rather than when somebody
	 * looks: a board is a thing that arrives. A descriptor comes again whenever a
	 * field changes, which neither re-subscribes nor brings back a board left.
	 */
	const bool following = at->subscribed.count(board.name) > 0;
	if (!following && !at->left.count(board.name) && board.may(BoardPermission::Subscribe) && held
	    && follow(*at, board.name, held->index.cursor(at->key, board.name)))
		return;

	announceState(at->key, board.name, following, false);
}

void QuickDC::BoardKeeper::refused(Api::HubHandle hub, int code, const std::string& board, const std::string&)
{
	Session* at = session(hub);
	if (!at || board.empty() || code != ADC_CODE_BULLETIN_NO_BOARD) return;

	at->described.erase(board);
	at->subscribed.erase(board);
	announceState(at->key, board, false, true);
}

void QuickDC::BoardKeeper::arrived(Api::HubHandle hub, const BoardEntry& entry)
{
	Session* at = session(hub);
	Store* here = store();
	if (!at || !here || entry.board.empty()) return;

	const std::string& key = at->key;
	if (!here->index.append(key, entry.board, entry)) return;

	arriving[{key, entry.board}].push_back(entry);
	if (++arrivingCount >= BATCH_ENTRIES) flush();
	else if (!batch) batch = new Samurai::Timer(core.loop(), this, std::chrono::milliseconds(BATCH_MS), true);

	if (entry.removed)
	{
		here->posts.forget(entry.tth);
		written.erase(entry.tth);
		tell(entry.tth, State::Withdrawn, Why::Withdrawn);
		release();
		return;
	}

	if (here->posts.verification(entry.tth) == PostStore::Verification::Failed)
		tell(entry.tth, State::Unavailable, Why::KnownBad);

	if (serves(key) && here->posts.serve(entry.tth)) fetchedFrom.emplace(entry.tth, key);

	/* A post written here is taken into the store as soon as the hub has said it is one. */
	const auto mine = written.find(entry.tth);
	if (mine == written.end()) return;

	const std::string copy = home + "/incoming/" + entry.tth;
	std::error_code error;
	std::filesystem::copy_file(mine->second, copy, std::filesystem::copy_options::overwrite_existing, error);
	written.erase(mine);
	if (!error) verify(key, entry, copy);
}

void QuickDC::BoardKeeper::flush()
{
	if (!held || arriving.empty()) return;

	std::map<std::pair<std::string, std::string>, std::vector<BoardEntry>> told;
	told.swap(arriving);
	arrivingCount = 0;

	for (auto& [where, entries] : told)
	{
		Api::Events::BoardEntries batched;
		batched.hubUrl = where.first;
		batched.board = where.second;
		batched.unread = static_cast<int>(held->index.unread(where.first, where.second));
		for (const BoardEntry& entry : entries)
			if (held->index.isRead(where.first, where.second, entry.tth)) batched.read.push_back(entry.tth);
		batched.entries = std::move(entries);
		publish(batched);

		/* A replay reaching back to the hub's oldest closes what looked like a gap. */
		const auto said = views.find(where);
		if (said != views.end() && said->second != converted(held->index.view(where.first, where.second)))
		{
			Session* at = sessionFor(where.first);
			announceState(where.first, where.second, at && at->subscribed.count(where.second), false);
		}
	}
}

void QuickDC::BoardKeeper::offerBoard(const std::string& hubKey, const std::string& board)
{
	Store* here = store();
	if (!here || !serves(hubKey)) return;

	for (const BoardEntry& entry : here->index.entries(hubKey, board))
	{
		if (entry.removed) continue;
		if (here->posts.serve(entry.tth)) fetchedFrom.emplace(entry.tth, hubKey);
	}
}

void QuickDC::BoardKeeper::load(const std::string& hubUrl, const std::string& board)
{
	Store* here = store();
	if (board.empty()) return;

	flush();

	const std::string key = hubSettingsGroup(hubUrl);

	Api::Events::BoardEntries told;
	told.hubUrl = key;
	told.board = board;
	told.whole = true;

	if (here)
	{
		told.entries = here->index.entries(key, board);
		told.unread = static_cast<int>(here->index.unread(key, board));
		for (const BoardEntry& entry : told.entries)
			if (here->index.isRead(key, board, entry.tth)) told.read.push_back(entry.tth);
	}

	publish(told);

	Session* at = sessionFor(key);
	announceState(key, board, at && at->subscribed.count(board), false);

	if (!here) return;

	offerBoard(key, board);
	for (const BoardEntry& entry : told.entries)
		if (!entry.removed && here->posts.verification(entry.tth) == PostStore::Verification::Failed)
			tell(entry.tth, State::Unavailable, Why::KnownBad);
}

void QuickDC::BoardKeeper::subscribe(Api::HubHandle hub, const std::string& board, bool fromStart)
{
	Session* at = session(hub);
	Store* here = store();
	if (!at || !here || board.empty() || !at->available) return;

	at->left.erase(board);
	if (at->subscribed.count(board) && !fromStart) return;

	if (!follow(*at, board, fromStart ? 0 : here->index.cursor(at->key, board)))
		announceState(at->key, board, false, false);
}

void QuickDC::BoardKeeper::unsubscribe(Api::HubHandle hub, const std::string& board)
{
	Session* at = session(hub);
	if (!at || board.empty()) return;

	if (at->link && at->available) at->link->unsubscribeBoard(board);
	at->subscribed.erase(board);
	at->left.insert(board);
	announceState(at->key, board, false, false);
}

void QuickDC::BoardKeeper::markRead(const std::string& hubUrl, const std::string& board, const std::string& tth)
{
	Store* here = store();
	if (!here || board.empty()) return;

	flush();

	const std::string key = hubSettingsGroup(hubUrl);
	BoardIndex& index = here->index;

	Api::Events::BoardRead told;
	told.hubUrl = key;
	told.board = board;

	if (tth.empty())
	{
		index.markAllRead(key, board);
		told.whole = true;
		for (const BoardEntry& entry : index.entries(key, board))
			if (index.isRead(key, board, entry.tth)) told.read.push_back(entry.tth);
	}
	else
	{
		if (!index.markRead(key, board, tth)) return;
		told.read.push_back(tth);
	}

	told.unread = static_cast<int>(index.unread(key, board));
	publish(told);
}

std::vector<QuickDC::BoardEntry> QuickDC::BoardKeeper::threadOf(const std::string& hubKey,
	const std::string& board, const std::string& tth)
{
	std::vector<BoardEntry> thread;
	Store* here = store();
	if (!here) return thread;

	const std::vector<BoardEntry> entries = here->index.entries(hubKey, board);
	std::map<std::string, const BoardEntry*> by;
	for (const BoardEntry& entry : entries) by[entry.tth] = &entry;

	/* Up by the parent, which is inside the hash; by the hub's thread where the parent is not held. */
	const auto rootOf = [&by](std::string at) {
		std::set<std::string> seen;
		for (;;)
		{
			seen.insert(at);
			const auto found = by.find(at);
			if (found == by.end()) return at;

			const BoardEntry& entry = *found->second;
			std::string up;
			if (!entry.parent.empty() && by.count(entry.parent)) up = entry.parent;
			else if (!entry.parent.empty() && !entry.thread.empty() && by.count(entry.thread)) up = entry.thread;

			if (up.empty() || seen.count(up)) return at;
			at = up;
		}
	};

	const std::string root = rootOf(tth);
	for (const BoardEntry& entry : entries)
		if (!entry.removed && entry.tth != tth && rootOf(entry.tth) == root) thread.push_back(entry);

	return thread;
}

void QuickDC::BoardKeeper::read(const std::string& hubUrl, const std::string& board, const std::string& tth,
                                bool fetchIt)
{
	Store* here = store();
	if (!here || board.empty() || tth.empty()) return;

	const std::string key = hubSettingsGroup(hubUrl);

	BoardEntry entry;
	bool found = false;
	for (const BoardEntry& one : here->index.entries(key, board))
		if (one.tth == tth) { entry = one; found = true; break; }
	if (!found) return;

	/* Read means opened, which is not fetched: a post nobody could be found for was still read. */
	markRead(key, board, tth);

	if (!fetchIt)
	{
		/* Reading a file already on this disk tells nobody anything, so it needs no asking. */
		if (entry.removed || !ADC::isValidTTH(tth) || fetching.count(tth)) return;
		if (here->posts.verification(tth) == PostStore::Verification::Failed)
			tell(tth, State::Unreadable, Why::KnownBad);
		else if (here->posts.has(tth))
			verify(key, entry, here->posts.pathFor(tth));
		return;
	}

	fetch(key, entry);
	if (!fetchesAhead(key)) return;

	for (const BoardEntry& more : threadOf(key, board, tth))
	{
		if (here->posts.has(more.tth)) continue;
		const auto known = states.find(more.tth);
		if (known != states.end() && (known->second == State::Verified || known->second == State::Disputed)) continue;
		fetch(key, more);
	}
}

void QuickDC::BoardKeeper::fetch(const std::string& hubKey, const BoardEntry& entry)
{
	Store* here = store();

	if (!ADC::isValidTTH(entry.tth))
	{
		tell(entry.tth, State::Unavailable, Why::NotAHash);
		return;
	}

	if (entry.removed)
	{
		tell(entry.tth, State::Withdrawn, Why::Withdrawn);
		return;
	}

	if (fetching.count(entry.tth)) return;
	for (const auto& queued : waiting)
		if (queued.second.tth == entry.tth) return;

	if (!here)
	{
		tell(entry.tth, State::Unavailable, Why::NoStore);
		return;
	}

	if (here->posts.verification(entry.tth) == PostStore::Verification::Failed)
	{
		tell(entry.tth, State::Unreadable, Why::KnownBad);
		return;
	}

	if (here->posts.has(entry.tth))
	{
		verify(hubKey, entry, here->posts.pathFor(entry.tth));
		return;
	}

	start(hubKey, entry);
}

void QuickDC::BoardKeeper::start(const std::string& hubKey, const BoardEntry& entry)
{
	/* The declared size is the author's claim; the cap is this client's own. */
	if (entry.size > FETCH_CAP)
	{
		tell(entry.tth, State::Unavailable, Why::TooLarge, entry.size);
		return;
	}

	if (fetching.size() >= CONCURRENT_FETCHES)
	{
		/* Here rather than in the queue, so a thread opened does not take the slots of what was asked for. */
		waiting.emplace_back(hubKey, entry);
		return;
	}

	if (!core.queue)
	{
		tell(entry.tth, State::Unavailable, Why::NoStore);
		return;
	}

	Fetch going;
	going.hub = hubKey;
	going.entry = entry;
	going.path = home + "/incoming/" + entry.tth;
	going.cap = entry.size > 0 ? entry.size : FETCH_CAP;

	if (!withinAllowance(hubKey, entry.board, going.cap))
	{
		tell(entry.tth, State::Unavailable, Why::Allowance);
		return;
	}

	QueueFile* file = core.queue->add(going.path.c_str(), entry.tth.c_str(), going.cap);
	if (!file)
	{
		tell(entry.tth, State::Unavailable, Why::NoStore);
		return;
	}

	going.since = clock.steadyMs();
	fetching[entry.tth] = going;
	tell(entry.tth, State::Fetching, Why::None);

	/* The hub it was read on is where it is asked for; the author is named later, if at all. */
	file->searchOnlyOn(hubKey.c_str());
	core.queue->requestSourceSearch(entry.tth.c_str());
	watch();
}

void QuickDC::BoardKeeper::startWaiting()
{
	while (!waiting.empty() && fetching.size() < CONCURRENT_FETCHES)
	{
		const std::pair<std::string, BoardEntry> next = waiting.front();
		waiting.pop_front();
		start(next.first, next.second);
	}
}

void QuickDC::BoardKeeper::giveUp(const std::string& tth, State state, Why why, uint64_t size)
{
	const auto found = fetching.find(tth);
	if (found != fetching.end())
	{
		const std::string path = found->second.path;
		fetching.erase(found);

		/* What came is named after a hash it did not turn out to have, and worth nothing to the next try. */
		if (core.queue)
			if (QueueFile* file = core.queue->getFileByName(path.c_str()))
			{
				std::string partial;
				if (file->getTempFilename()) partial = file->getTempFilename();
				core.queue->remove(file);
				if (!partial.empty()) Samurai::IO::File(partial).remove();
			}
	}

	tell(tth, state, why, size);
	startWaiting();
}

bool QuickDC::BoardKeeper::withinAllowance(const std::string& hubKey, const std::string& board, uint64_t bytes)
{
	const uint64_t now = clock.steadyMs();
	Spent& spent = allowances[boardKey(hubKey, board)];

	if (spent.since == 0 || now - spent.since > ALLOWANCE_MS)
	{
		spent.since = now ? now : 1;
		spent.bytes = 0;
	}

	if (spent.bytes + bytes > ALLOWANCE_BYTES) return false;

	spent.bytes += bytes;
	return true;
}

void QuickDC::BoardKeeper::tell(const std::string& tth, State state, Why why, uint64_t size)
{
	if (tth.empty()) return;

	/* A tombstone says more of a post than anything that could be said of its document. */
	const auto known = states.find(tth);
	if (known != states.end() && known->second == State::Withdrawn && state != State::Withdrawn) return;

	states[tth] = state;

	Api::Events::PostStatus told;
	told.tth = tth;
	told.state = state;
	told.why = why;
	told.size = size;
	publish(told);
}

bool QuickDC::BoardKeeper::downloaded(const std::string& filename, const std::string& tth)
{
	for (const auto& [hash, going] : fetching)
	{
		if (going.path != filename) continue;

		const Fetch fetched = going;
		fetching.erase(hash);

		if (!tth.empty() && tth != fetched.entry.tth)
		{
			Samurai::IO::File(filename).remove();
			tell(fetched.entry.tth, State::Unavailable, Why::Mismatch);
		}
		else
		{
			verify(fetched.hub, fetched.entry, filename);
		}

		startWaiting();
		return true;
	}

	return false;
}

void QuickDC::BoardKeeper::verify(const std::string& hubKey, const BoardEntry& entry, const std::string& path)
{
	Store* here = store();
	if (!here)
	{
		tell(entry.tth, State::Unavailable, Why::NoStore);
		return;
	}

	PostStore& posts = here->posts;
	const std::string& tth = entry.tth;

	/* The store's own copy, rather than one a download has just left. */
	const bool ours = posts.has(tth) && posts.pathFor(tth) == path;

	std::ifstream document(path, std::ios::binary);
	if (!document)
	{
		tell(tth, ours ? State::Unreadable : State::Unavailable, Why::Unreadable);
		return;
	}

	/* One byte past the cap, so a document of exactly the cap is not mistaken for more. */
	std::string bytes(static_cast<size_t>(FETCH_CAP) + 1, '\0');
	document.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	bytes.resize(static_cast<size_t>(document.gcount()));
	document.close();

	if (bytes.size() > FETCH_CAP)
	{
		if (!ours) Samurai::IO::File(path).remove();
		tell(tth, ours ? State::Unreadable : State::Unavailable, Why::OverCap);
		return;
	}

	if (tigerTreeRoot(bytes) != tth)
	{
		/* A copy held is kept and marked, so the same bytes are not fetched again. */
		if (ours) posts.record(tth, PostStore::Verification::Failed);
		else Samurai::IO::File(path).remove();
		tell(tth, ours ? State::Unreadable : State::Unavailable, Why::WrongHash);
		return;
	}

	Post parsed;
	if (!ADC::parsePost(bytes.data(), bytes.size(), parsed))
	{
		const bool adopted = ours || posts.adopt(tth, path);
		posts.record(tth, PostStore::Verification::Failed);
		tell(tth, adopted ? State::Unreadable : State::Unavailable, Why::NotAPost);
		return;
	}

	if (!ours) posts.adopt(tth, path);
	posts.record(tth, PostStore::Verification::Verified);
	if (serves(hubKey)) posts.serve(tth);
	fetchedFrom.emplace(tth, hubKey);

	Api::Events::PostStatus told;
	told.tth = tth;
	told.state = parsed.cid == entry.cid ? State::Verified : State::Disputed;
	told.post.tth = tth;
	told.post.cid = parsed.cid;
	told.post.parent = parsed.parent;
	told.post.subject = parsed.subject;
	told.post.body = parsed.body;
	told.post.dated = parsed.date.has_value();
	told.post.date = parsed.date.value_or(0);
	told.post.richText = parsed.isRichText();
	told.attributedTo = entry.cid;

	states[tth] = told.state;
	publish(told);
}

void QuickDC::BoardKeeper::write(const Api::Commands::WritePost& command)
{
	using Refused = Api::Events::PostRefused;

	Session* at = session(command.hub);
	if (!at || !at->link || !at->available || command.board.empty())
	{
		publish(Refused{command.request, Refused::Why::NoHub, 0, 0, std::string()});
		return;
	}

	const char* cid = core.identity ? core.identity->getCID() : nullptr;
	if (!cid || !*cid)
	{
		publish(Refused{command.request, Refused::Why::NoIdentity, 0, 0, std::string()});
		return;
	}

	const std::string subject = oneLine(command.subject);
	if (subject.empty() && command.parent.empty())
	{
		publish(Refused{command.request, Refused::Why::NoSubject, 0, 0, std::string()});
		return;
	}

	Post post;
	post.cid = cid;
	post.parent = command.parent;
	post.subject = subject;
	post.date = clock.epochSeconds();
	if (command.richText) post.richTextValue = 1;

	const std::string document = ADC::writePostHeader(post) + "\n" + command.body;

	/* The hub enforces what its descriptor says against the size declared; no reason to learn it by refusal. */
	const auto board = at->described.find(command.board);
	const uint64_t limit = board == at->described.end() ? 0 : board->second.maxSize;
	if (limit > 0 && document.size() > limit)
	{
		publish(Refused{command.request, Refused::Why::TooLarge, document.size(), limit, std::string()});
		return;
	}

	if (home.empty() || !core.shares)
	{
		publish(Refused{command.request, Refused::Why::NotWritten, 0, 0, "there is nowhere to keep it"});
		return;
	}

	/* Until a reader has fetched it its author is its only source, so it is written down first. */
	const std::string folder = home + "/outbox";
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	if (error)
	{
		publish(Refused{command.request, Refused::Why::NotWritten, 0, 0, folder + ": " + error.message()});
		return;
	}

	char stamp[32];
	const time_t now = static_cast<time_t>(clock.epochSeconds());
	struct tm local;
	localtime_r(&now, &local);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

	std::string path = folder + "/post-" + stamp + "-" + std::to_string(command.request);
	for (int n = 1; std::filesystem::exists(path, error); ++n)
		path = folder + "/post-" + stamp + "-" + std::to_string(command.request) + "-" + std::to_string(n);

	std::ofstream out(path, std::ios::binary);
	out.write(document.data(), static_cast<std::streamsize>(document.size()));
	out.close();
	if (!out)
	{
		publish(Refused{command.request, Refused::Why::NotWritten, 0, 0, path + " could not be written"});
		return;
	}

	writing[path] = Writing{command, at->key, subject, path};

	/* For the session only: whether a post is offered is decided again at each start from what is held. */
	if (!core.shares->attach(path.c_str(), this, Share::Manager::Persistence::Session, Share::Manager::SlotCost::Free))
	{
		writing.erase(path);
		publish(Refused{command.request, Refused::Why::NotOffered, 0, 0, path + " could not be offered"});
	}
}

void QuickDC::BoardKeeper::EventAttachmentReady(const char* path, const char* tth, uint64_t size)
{
	const auto found = writing.find(path ? path : "");
	if (found == writing.end()) return;

	const Writing done = found->second;
	writing.erase(found);

	Session* at = session(done.command.hub);
	if (!at || !at->link
	    || !at->link->submitPost(tth, size, done.command.board, done.subject, done.command.parent))
	{
		publish(Api::Events::PostRefused{done.command.request, Api::Events::PostRefused::Why::NoHub, 0, 0, std::string()});
		return;
	}

	written[tth] = done.path;
	publish(Api::Events::PostSubmitted{done.command.request, done.command.board, tth, size});
}

void QuickDC::BoardKeeper::EventAttachmentFailed(const char* path, const char* reason)
{
	const auto found = writing.find(path ? path : "");
	if (found == writing.end()) return;

	const uint64_t request = found->second.command.request;
	writing.erase(found);
	publish(Api::Events::PostRefused{request, Api::Events::PostRefused::Why::NotOffered, 0, 0, reason ? reason : ""});
}

void QuickDC::BoardKeeper::policyChanged()
{
	if (!held) return;

	std::set<std::string> hubs;
	for (const auto& [handle, at] : sessions) hubs.insert(at.key);
	for (const auto& [tth, hub] : fetchedFrom) hubs.insert(hub);

	for (const std::string& hub : hubs)
	{
		const bool now = serves(hub);
		const auto was = offering.find(hub);
		if (was != offering.end() && was->second == now) continue;
		offering[hub] = now;

		/* The documents read from this hub; a post read on two follows the first. */
		for (const auto& [tth, from] : fetchedFrom)
		{
			if (from != hub) continue;
			if (now) held->posts.serve(tth);
			else held->posts.withhold(tth);
		}

		if (now)
			if (Session* at = sessionFor(hub))
				for (const std::string& board : at->subscribed) offerBoard(hub, board);
	}

	evict();
}

void QuickDC::BoardKeeper::evict()
{
	if (!held || !core.config) return;

	PostStore::Bounds bounds;
	bounds.age = static_cast<uint64_t>(std::max(0, Settings::read(*core.config, Settings::BoardKeepDays))) * 24 * 60 * 60;
	bounds.total = static_cast<uint64_t>(std::max(0, Settings::read(*core.config, Settings::BoardCache)));

	if (bounds.age || bounds.total) held->posts.evict(bounds, clock.epochSeconds());
	release();
}

void QuickDC::BoardKeeper::release()
{
	if (!held) return;

	const std::vector<std::string> pending = held->posts.pending();
	if (pending.empty()) return;

	/* An upload reads through a handle withdrawing a hash destroys, so what is being sent waits. */
	std::set<std::string> reading;
	if (core.transfers)
		for (Transfer* transfer = core.transfers->first(); transfer; transfer = core.transfers->next())
			if (transfer->isUpload())
				if (Samurai::IO::File* file = transfer->getFile()) reading.insert(leafOf(file->getName()));

	bool waitingOnUploads = false;
	for (const std::string& tth : pending)
	{
		if (reading.count(tth))
		{
			waitingOnUploads = true;
			continue;
		}

		const bool going = held->posts.has(tth);
		if (!held->posts.release(tth) || !going || held->posts.has(tth)) continue;

		fetchedFrom.erase(tth);
		tell(tth, State::Announced, Why::Evicted);
	}

	if (waitingOnUploads) watch();
}

void QuickDC::BoardKeeper::transfersChanged()
{
	release();
}

void QuickDC::BoardKeeper::watch()
{
	if (!timer) timer = new Samurai::Timer(core.loop(), this, std::chrono::milliseconds(WATCH_MS), false);
}

void QuickDC::BoardKeeper::EventTimeout(Samurai::Timer* fired)
{
	if (fired == batch)
	{
		delete batch;
		batch = nullptr;
		flush();
		return;
	}

	tick();
}

void QuickDC::BoardKeeper::tick()
{
	const uint64_t now = clock.steadyMs();

	std::map<std::string, uint64_t> arrived;
	if (core.transfers)
	{
		const std::string_view suffix = QUICKDC_PART_SUFFIX;
		for (Transfer* transfer = core.transfers->first(); transfer; transfer = core.transfers->next())
		{
			if (transfer->isUpload()) continue;
			Samurai::IO::File* file = transfer->getFile();
			if (!file) continue;

			std::string name = file->getName();
			if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
				name.resize(name.size() - suffix.size());
			arrived[name] += transfer->getTransferred();
		}
	}

	std::vector<std::pair<std::string, Why>> lost;

	for (auto& [tth, going] : fetching)
	{
		const auto moving = arrived.find(going.path);
		if (moving != arrived.end() && moving->second > going.moved)
		{
			/* A fetch that is moving has a source, so there is nobody to name and nothing to give up on. */
			going.moved = moving->second;
			going.since = now;
		}

		if (going.moved > going.cap)
		{
			lost.emplace_back(tth, Why::Oversent);
			continue;
		}

		if (now - going.since >= FETCH_TIMEOUT_MS)
		{
			lost.emplace_back(tth, Why::NobodyAnswered);
			continue;
		}

		/*
		 * The search has had its chance. An author who records who fetches their post
		 * has a census of its readers, but a post nobody else holds is unreadable
		 * without them, and unreadable is worse than seen to be read.
		 */
		if (!going.authorNamed && now - going.since >= AUTHOR_GRACE_MS && core.queue)
		{
			going.authorNamed = true;
			QueueFile* file = core.queue->getFileByTTH(tth.c_str());
			if (file && file->countSources() == 0 && !going.entry.cid.empty())
				core.queue->addKnownSource(tth.c_str(), going.hub.c_str(), going.entry.cid.c_str());
		}
	}

	for (const auto& [tth, why] : lost) giveUp(tth, State::Unavailable, why);

	release();

	if (fetching.empty() && waiting.empty() && timer)
	{
		bool pendingUploads = false;
		if (held)
			for (const std::string& tth : held->posts.pending())
				if (held->posts.has(tth)) { pendingUploads = true; break; }

		if (!pendingUploads)
		{
			delete timer;
			timer = nullptr;
		}
	}
}
