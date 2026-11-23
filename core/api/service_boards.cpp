/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * The board stores a front end keeps, and what it asks of them.
 *
 * Moved here from the user interface's bridge, which used to build one on the
 * core thread and reach into it from jobs: the index and the documents are the
 * core's, and so is deciding whether a document is the post it was asked for.
 */

#include "api/service_boards.h"
#include "api/core.h"
#include "hash/hashjob.h"
#include "network/adc/adcbbs.h"
#include "network/transfer.h"
#include "network/transfermanager.h"

#include <samurai/crypto/digest/merkletree.h>
#include <samurai/crypto/digest/tiger.h>
#include <samurai/io/file.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

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

/* The last path component, which is all a held document is filed under. */
std::string leafOf(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

}

QuickDC::Api::Service::BoardStore* QuickDC::Api::Service::boardStore(BoardStoreHandle store)
{
	const auto found = boardStores.find(store);
	return found == boardStores.end() ? nullptr : found->second.get();
}

void QuickDC::Api::Service::run(const Commands::OpenBoardStore& command)
{
	if (boardStores.count(command.store)) return;

	std::error_code ignored;
	std::filesystem::create_directories(command.incomingDir, ignored);

	auto made = std::make_unique<BoardStore>(command.boardsDir, command.documentsDir, core.shares);

	/*
	 * Whatever was marked to go before the last shutdown goes now. An offer to
	 * the share manager lasts no longer than the process that made it, so a store
	 * that has just been opened is serving nothing and nothing can be reading one.
	 */
	for (const std::string& tth : made->posts.pending()) made->posts.release(tth);

	boardStores[command.store] = std::move(made);
}

void QuickDC::Api::Service::run(const Commands::CloseBoardStore& command)
{
	boardStores.erase(command.store);
}

void QuickDC::Api::Service::run(const Commands::StoreBoardEntries& command)
{
	Events::BoardEntriesStored stored{command.store, command.request, {}, {}, {}, {}, {}};

	if (BoardStore* store = boardStore(command.store))
	{
		std::map<std::string, std::vector<std::string>> touched;

		for (size_t n = 0; n < command.entries.size(); ++n)
		{
			const BoardEntry& entry = command.entries[n];
			if (entry.board.empty()) continue;
			if (!store->index.append(command.hubUrl, entry.board, entry)) continue;

			stored.kept.push_back(static_cast<int>(n));
			touched[entry.board].push_back(entry.tth);

			if (store->posts.verification(entry.tth) == PostStore::Verification::Failed)
				stored.failed.push_back(entry.tth);
			else if (store->posts.has(entry.tth))
				stored.held.push_back(entry.tth);

			if (entry.removed)
			{
				store->posts.forget(entry.tth);
				continue;
			}

			if (command.serving && store->posts.serve(entry.tth))
				stored.offered.push_back(entry.tth);
		}

		for (const auto& [board, written] : touched)
		{
			BoardReadState where;
			where.board = board;
			where.unread = static_cast<int>(store->index.unread(command.hubUrl, board));
			for (const std::string& tth : written)
				if (store->index.isRead(command.hubUrl, board, tth)) where.read.push_back(tth);
			stored.boards.push_back(std::move(where));
		}
	}

	publish(stored);
}

void QuickDC::Api::Service::run(const Commands::DescribeBoard& command)
{
	Events::BoardStateReported state{command.store, command.request, 0, BoardView::Unknown};

	if (BoardStore* store = boardStore(command.store))
	{
		store->index.describe(command.hubUrl, command.board);
		state.cursor = store->index.cursor(command.hubUrl, command.board.name);
		state.view = converted(store->index.view(command.hubUrl, command.board.name));
	}

	publish(state);
}

void QuickDC::Api::Service::run(const Commands::QueryBoardState& command)
{
	Events::BoardStateReported state{command.store, command.request, 0, BoardView::Unknown};

	if (BoardStore* store = boardStore(command.store))
	{
		state.cursor = store->index.cursor(command.hubUrl, command.board);
		state.view = converted(store->index.view(command.hubUrl, command.board));
	}

	publish(state);
}

void QuickDC::Api::Service::run(const Commands::LoadBoard& command)
{
	Events::BoardLoaded loaded;
	loaded.store = command.store;
	loaded.request = command.request;

	if (BoardStore* store = boardStore(command.store))
	{
		loaded.entries = store->index.entries(command.hubUrl, command.board);

		for (const BoardEntry& entry : loaded.entries)
		{
			if (store->index.isRead(command.hubUrl, command.board, entry.tth))
				loaded.read.push_back(entry.tth);

			if (store->posts.verification(entry.tth) == PostStore::Verification::Failed)
				loaded.failed.push_back(entry.tth);
			else if (store->posts.has(entry.tth))
				loaded.held.push_back(entry.tth);

			if (command.serving && store->posts.serve(entry.tth))
				loaded.offered.push_back(entry.tth);
		}

		loaded.unread = static_cast<int>(store->index.unread(command.hubUrl, command.board));
	}

	publish(loaded);
}

void QuickDC::Api::Service::run(const Commands::ResumeBoard& command)
{
	Events::BoardResumed resumed{command.store, command.request, 0, BoardView::Unknown, {}};

	if (BoardStore* store = boardStore(command.store))
	{
		resumed.cursor = store->index.cursor(command.hubUrl, command.board);
		resumed.view = converted(store->index.view(command.hubUrl, command.board));

		if (command.serving)
		{
			for (const BoardEntry& entry : store->index.entries(command.hubUrl, command.board))
			{
				if (entry.removed) continue;
				if (store->posts.serve(entry.tth)) resumed.offered.push_back(entry.tth);
			}
		}
	}

	publish(resumed);
}

void QuickDC::Api::Service::run(const Commands::MarkRead& command)
{
	Events::BoardReadChanged changed{command.store, command.request, {}};
	changed.state.board = command.board;

	if (BoardStore* store = boardStore(command.store))
	{
		BoardIndex& index = store->index;

		if (command.tth.empty())
		{
			index.markAllRead(command.hubUrl, command.board);
			for (const BoardEntry& entry : index.entries(command.hubUrl, command.board))
				if (index.isRead(command.hubUrl, command.board, entry.tth))
					changed.state.read.push_back(entry.tth);
		}
		else
		{
			index.markRead(command.hubUrl, command.board, command.tth);
			if (index.isRead(command.hubUrl, command.board, command.tth))
				changed.state.read.push_back(command.tth);
		}

		changed.state.unread = static_cast<int>(index.unread(command.hubUrl, command.board));
	}

	publish(changed);
}

void QuickDC::Api::Service::run(const Commands::ServePosts& command)
{
	BoardStore* store = boardStore(command.store);
	if (!store) return;

	for (const std::string& tth : command.tths)
	{
		if (command.serving) store->posts.serve(tth);
		else store->posts.withhold(tth);
	}
}

void QuickDC::Api::Service::run(const Commands::QueryPost& command)
{
	Events::PostKnown known{command.store, command.request, std::string(), false};

	if (BoardStore* store = boardStore(command.store))
	{
		known.failed = store->posts.verification(command.tth) == PostStore::Verification::Failed;
		if (!known.failed && store->posts.has(command.tth))
			known.path = store->posts.pathFor(command.tth);
	}

	publish(known);
}

void QuickDC::Api::Service::run(const Commands::AdoptPost& command)
{
	using Outcome = Events::PostAdopted::Outcome;

	Events::PostAdopted verdict;
	verdict.store = command.store;
	verdict.request = command.request;
	verdict.post.tth = command.entry.tth;

	BoardStore* store = boardStore(command.store);
	if (!store)
	{
		verdict.outcome = Outcome::NoStore;
		publish(verdict);
		return;
	}

	PostStore& posts = store->posts;
	const std::string& file = command.path;
	const std::string& tth = command.entry.tth;

	/* The store's own copy, rather than one a download has just left. */
	const bool held = posts.has(tth) && !file.empty() && posts.pathFor(tth) == file;
	verdict.held = held;

	std::ifstream document(file, std::ios::binary);
	if (!document)
	{
		verdict.outcome = Outcome::Unreadable;
		publish(verdict);
		return;
	}

	/* One byte past the cap, so that a document of exactly the cap is not mistaken for more. */
	std::string bytes;
	if (command.cap > 0)
	{
		bytes.resize(static_cast<size_t>(command.cap) + 1);
		document.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		bytes.resize(static_cast<size_t>(document.gcount()));
	}
	else
	{
		bytes.assign(std::istreambuf_iterator<char>(document), std::istreambuf_iterator<char>());
	}
	document.close();

	if (command.cap > 0 && bytes.size() > command.cap)
	{
		verdict.outcome = Outcome::TooLarge;
		publish(verdict);
		return;
	}

	if (tigerTreeRoot(bytes) != tth)
	{
		/* A held copy is kept and marked, so the same bytes are not fetched again. */
		if (held) posts.record(tth, PostStore::Verification::Failed);
		else Samurai::IO::File(file).remove();

		verdict.outcome = Outcome::WrongHash;
		publish(verdict);
		return;
	}

	Post parsed;
	if (!ADC::parsePost(bytes.data(), bytes.size(), parsed))
	{
		if (!held) verdict.held = posts.adopt(tth, file);
		posts.record(tth, PostStore::Verification::Failed);

		verdict.outcome = Outcome::NotAPost;
		publish(verdict);
		return;
	}

	verdict.post.cid = parsed.cid;
	verdict.post.parent = parsed.parent;
	verdict.post.subject = parsed.subject;
	verdict.post.body = parsed.body;
	verdict.post.dated = parsed.date.has_value();
	verdict.post.date = parsed.date.value_or(0);
	verdict.post.richText = parsed.isRichText();

	verdict.outcome = parsed.cid == command.entry.cid ? Outcome::Verified : Outcome::Disputed;

	if (!held) verdict.held = posts.adopt(tth, file);
	posts.record(tth, PostStore::Verification::Verified);
	if (command.serving) posts.serve(tth);

	publish(verdict);
}

void QuickDC::Api::Service::run(const Commands::EvictPosts& command)
{
	Events::PostsEvicted evicted{command.store, command.request, 0};

	if (BoardStore* store = boardStore(command.store))
	{
		PostStore::Bounds bounds;
		bounds.age = command.age;
		bounds.total = command.total;
		evicted.marked = static_cast<int>(store->posts.evict(bounds, command.now).size());
	}

	publish(evicted);
}

/*
 * A document is filed under its hash and nothing else, so a file being sent
 * names the document it holds by its last path component. Anything else being
 * uploaded is a shared file, and not this store's to withdraw.
 */
void QuickDC::Api::Service::run(const Commands::ReleasePosts& command)
{
	BoardStore* store = boardStore(command.store);
	if (!store) return;

	std::set<std::string> reading;
	for (const std::string& name : command.sending) reading.insert(leafOf(name));

	if (core.transfers)
	{
		for (QuickDC::Transfer* transfer = core.transfers->first(); transfer;
		     transfer = core.transfers->next())
		{
			if (!transfer->isUpload()) continue;
			if (Samurai::IO::File* file = transfer->getFile()) reading.insert(leafOf(file->getName()));
		}
	}

	for (const std::string& tth : store->posts.pending())
		if (!reading.count(tth)) store->posts.release(tth);
}
