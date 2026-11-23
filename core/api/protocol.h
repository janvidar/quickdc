/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_PROTOCOL_H
#define HAVE_QUICKDC_API_PROTOCOL_H

/*
 * What crosses between the core and a front end.
 *
 * Everything here is a plain value: no pointer into the core, nothing that is
 * only valid until the next turn of its loop. A front end sends a Command with
 * Core::submit() from any thread, and is told what happened by an Event handed
 * to the EventSink it subscribed, on the core's thread - where it copies the
 * value to wherever it is wanted. Nothing a front end holds can dangle when the
 * core moves on or goes away.
 *
 * Grown one front-end concern at a time; see docs/redesign.md.
 */

#include "api/bulletins.h"
#include "api/dropverdict.h"
#include "api/hublistentry.h"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace QuickDC {
namespace Api {

struct RemoteFile {
	std::string name;
	uint64_t size = 0;
	/** Empty where the list did not carry one. */
	std::string tth;
};

struct RemoteDirectory {
	std::string name;
	uint64_t size = 0;
	uint64_t fileCount = 0;
	std::vector<RemoteFile> files;
	std::vector<RemoteDirectory> dirs;
};

/** What a download is written to until it is verified: the name plus this. */
inline constexpr char PART_SUFFIX[] = ".part";

enum class Priority { Paused, Low, Normal, High };

/** One transfer - a single chunk on one connection - as it stands. */
struct Transfer {
	/** Stable for the transfer's life, and only for that. */
	uint64_t id = 0;
	std::string filename;
	uint64_t transferred = 0;
	uint64_t length = 0;
	uint64_t bytesPerSecond = 0;
	/** Bytes on the wire per byte of file; below one where compressed. */
	double ratio = 1.0;
	bool upload = false;
	bool finished = false;
};

/** One file in the download queue. */
struct QueueItem {
	std::string filename;
	std::string tth;
	uint64_t size = 0;
	uint64_t done = 0;
	uint32_t sources = 0;
	uint32_t activeSources = 0;
	Priority priority = Priority::Normal;
	bool complete = false;
};

/**
 * A hub session as a front end names it: a number it picks when it opens one,
 * so two openings of the same address are two sessions, and an address typed
 * one way is never confused with the core's canonical spelling of it.
 */
using HubHandle = uint64_t;

enum class NetStatus {
	Invalid, Lookup, Connecting, Connected, Disconnected, TlsConnected,
	TlsDisconnected, ErrorTimeout, ErrorConnectionRefused, ErrorHostNotFound,
	ErrorHostUnreachable, ErrorIO, TlsError
};

enum class HubStatus {
	None, Login, Password, LoggedIn, ErrorLoginRefused, ErrorFull, ErrorDisabled,
	ErrorNickNotAccepted, ErrorNickTaken, ErrorCidTaken, ErrorWrongPassword,
	Kicked, BannedTemporarily, BannedPermanently, Redirected
};

struct User {
	std::string id;
	std::string nick;
	std::string description;
	std::string email;
	std::string speed;
	std::string userAgent;
	std::string addressIPv4;
	std::string addressIPv6;
	std::string keyprint;
	/** What a download source names this user by: the CID on ADC, the nick on NMDC. */
	std::string sourceId;
	uint64_t sharedBytes = 0;
	uint32_t sharedFiles = 0;
	uint32_t slotCount = 0;
	bool op = false;
	bool away = false;
};

struct ChatLine {
	std::string hubUrl;
	/** Empty when the hub itself spoke; see 'fromHub'. */
	User from;
	std::string fromHub;
	std::string text;
	/** The private conversation this belongs to; empty in main chat. */
	std::string context;
	/** Seconds since the epoch; 0 where the hub did not say. */
	uint64_t timestamp = 0;
	bool action = false;
	bool richText = false;
	bool privateMessage = false;
};

struct SearchHit {
	std::string hubUrl;
	std::string token;
	std::string name;
	std::string tth;
	uint64_t size = 0;
	uint32_t freeSlots = 0;
	User user;
};

/**
 * A board store a front end keeps posts and indexes in, named the way a hub
 * session is: by a number the front end picks when it opens one.
 */
using BoardStoreHandle = uint64_t;

/** Whether everything before what is held of a board will still be replayed. */
enum class BoardView { Unknown, Complete, Gap };

/** A post's document, parsed, as far as the store could vouch for it. */
struct PostDocument {
	std::string tth;
	std::string cid;
	std::string parent;
	std::string subject;
	std::string body;
	bool dated = false;
	uint64_t date = 0;
	bool richText = false;
};

struct BoardReadState {
	std::string board;
	int unread = 0;
	/** Which of the posts the request concerned are marked read. */
	std::vector<std::string> read;
};

namespace Commands {

/*
 * Every board store request names its store and carries a 'request' number
 * the answer echoes, since every sink hears every answer.
 */

struct OpenBoardStore {
	BoardStoreHandle store = 0;
	std::string boardsDir;
	std::string documentsDir;
	/** Where fetched documents land before they are adopted; made if absent. */
	std::string incomingDir;
};

/** Nothing is released: marks survive, and the next opening releases them. */
struct CloseBoardStore { BoardStoreHandle store = 0; };

/** Append what a hub sent to the index; answered by BoardEntriesStored. */
struct StoreBoardEntries {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::string hubUrl;
	std::vector<BoardEntry> entries;
	/** Offer the documents of these posts to anybody who asks. */
	bool serving = false;
};

/** Record a board's descriptor; answered by BoardStateReported. */
struct DescribeBoard {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::string hubUrl;
	Board board;
};

struct QueryBoardState {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::string hubUrl;
	std::string board;
};

/** Everything held of a board; answered by BoardLoaded. */
struct LoadBoard {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::string hubUrl;
	std::string board;
	bool serving = false;
};

/** Where to pick a subscription up from, offering what is held; BoardResumed. */
struct ResumeBoard {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::string hubUrl;
	std::string board;
	bool serving = false;
};

/** Answered by BoardReadChanged. An empty 'tth' marks the whole board. */
struct MarkRead {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::string hubUrl;
	std::string board;
	std::string tth;
};

struct ServePosts {
	BoardStoreHandle store = 0;
	std::vector<std::string> tths;
	bool serving = false;
};

/** What the store knows of one document; answered by PostKnown. */
struct QueryPost {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::string tth;
};

/**
 * Check the document at 'path' is the post 'entry' names, and keep it;
 * answered by PostAdopted. 'cap' bounds what is read, 0 for no bound.
 */
struct AdoptPost {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::string path;
	BoardEntry entry;
	bool serving = false;
	uint64_t cap = 0;
};

/** Mark documents past either bound to go; answered by PostsEvicted. */
struct EvictPosts {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	uint64_t age = 0;
	uint64_t total = 0;
	uint64_t now = 0;
};

/**
 * Let go of every document marked to go that nobody is being sent. 'sending'
 * names files a front end knows are going out, beside what the core sees.
 */
struct ReleasePosts {
	BoardStoreHandle store = 0;
	std::vector<std::string> sending;
};

/** Start a session for the hub at 'url', not yet connected; see ConnectHub. */
struct OpenHub {
	HubHandle hub = 0;
	std::string url;
};

/** Disconnect the session and forget it. */
struct CloseHub { HubHandle hub = 0; };
struct ConnectHub { HubHandle hub = 0; };
struct DisconnectHub { HubHandle hub = 0; };

/** What a session agrees to when a hub asks; both are read as the hub asks. */
struct SetHubPolicy {
	HubHandle hub = 0;
	bool followRedirects = false;
	bool allowPeerConnections = true;
};

struct SendChat {
	HubHandle hub = 0;
	std::string text;
	bool richText = false;
};

struct SendPrivateChat {
	HubHandle hub = 0;
	std::string userId;
	std::string text;
	bool richText = false;
};

struct SendPassword {
	HubHandle hub = 0;
	std::string password;
};

/** The last 'limit' lines of a conversation from the chat archive. */
struct RequestHistory {
	HubHandle hub = 0;
	std::string hubUrl;
	std::string conversation;
	int limit = 0;
};

struct SubscribeBoard {
	HubHandle hub = 0;
	std::string board;
	uint64_t timestamp = 0;
};

struct UnsubscribeBoard {
	HubHandle hub = 0;
	std::string board;
};

struct RequestBoardEntry {
	HubHandle hub = 0;
	std::string board;
	std::string tth;
};

struct SubmitPost {
	HubHandle hub = 0;
	std::string tth;
	uint64_t size = 0;
	std::string board;
	std::string subject;
	std::string parent;
};

struct WithdrawPost {
	HubHandle hub = 0;
	std::string tth;
	std::string board;
};

/** Download and parse the hublist at 'url'; one at a time per address. */
struct FetchHubList {
	std::string url;
};

/**
 * Browse what the user 'sourceId' shares on the hub at 'hubUrl'. Our own
 * share is read off this disk rather than asked of the hub.
 */
struct RequestFilelist {
	std::string hubUrl;
	std::string sourceId;
};

/**
 * Search every hub. 'token' is the caller's, and what results quote back; the
 * words are matched as given, 'exclude' ones must not appear, and a non-empty
 * 'extensions' limits what is asked for to those.
 */
struct Search {
	enum class Size { Any, AtLeast, AtMost };

	std::string token;
	std::vector<std::string> include;
	std::vector<std::string> exclude;
	std::vector<std::string> extensions;
	Size sizePolicy = Size::Any;
	uint64_t size = 0;
};

/** Queue 'filename' for download; an empty 'tth' leaves it to be found by name. */
struct Enqueue {
	std::string filename;
	std::string tth;
	uint64_t size = 0;
};

/** Queue 'filename' with the user who answered a search as its first source. */
struct EnqueueFromSearch {
	std::string filename;
	std::string tth;
	uint64_t size = 0;
	std::string hubUrl;
	std::string sourceId;
	/** The name the source has it under; may be empty. */
	std::string remoteName;
};

/**
 * Queue a file somebody named by hash so it can be read here, and look for a
 * source at once - only on 'onlyHub' where that is given.
 */
struct EnqueueForReading {
	std::string filename;
	std::string tth;
	uint64_t size = 0;
	std::string onlyHub;
};

/** Record a source for a queued 'tth' that has none, such as the one who named it. */
struct AddFallbackSource {
	std::string tth;
	std::string hubUrl;
	std::string sourceId;
};

struct SetPriority {
	std::string filename;
	Priority priority = Priority::Normal;
};

struct RemoveFromQueue {
	std::string filename;
	bool deletePartial = false;
};

/**
 * Report every transfer and queued file as a TransfersSnapshot.
 *
 * Every sink is told every snapshot, so 'request' is echoed back for the one
 * that asked to know its answer from one somebody else asked for earlier.
 */
struct SnapshotTransfers {
	uint64_t request = 0;
};

/** Measure reachability against the configured server, IPv4 first. */
struct MeasureReachability { };

/** Ask the configured server only what public address it sees. */
struct LookupPublicAddress {
	bool ipv6 = false;
};

/** Say again every verdict already held, as ReachabilityMeasured. */
struct ReplayReachability { };

/**
 * Start or stop the gateway mapping to match the connection mode.
 * Transitional: C2 has the core do this whenever a setting it depends on changes.
 */
struct ApplyPortForwarding { };

/**
 * Tell every hub again what it was told about this client at login.
 * Transitional: C4 has the core do this whenever any of it changes.
 */
struct RepublishClientInfo { };

/**
 * Offer the file at 'path' by its hash alone, for a message or a post to name.
 *
 * 'remembered' keeps the offer across restarts; a post's document is offered
 * for the session only. 'freeSlot' serves it without taking an upload slot.
 */
struct AttachFile {
	std::string path;
	bool remembered = true;
	bool freeSlot = false;
};

}

namespace Events {

struct BoardEntriesStored {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	/** Indices into the command's entries of the ones the index took. */
	std::vector<int> kept;
	std::vector<BoardReadState> boards;
	std::vector<std::string> held;
	std::vector<std::string> failed;
	std::vector<std::string> offered;
};

struct BoardStateReported {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	uint64_t cursor = 0;
	BoardView view = BoardView::Unknown;
};

struct BoardLoaded {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	std::vector<BoardEntry> entries;
	int unread = 0;
	std::vector<std::string> read;
	std::vector<std::string> held;
	std::vector<std::string> failed;
	std::vector<std::string> offered;
};

struct BoardResumed {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	uint64_t cursor = 0;
	BoardView view = BoardView::Unknown;
	std::vector<std::string> offered;
};

struct BoardReadChanged {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	BoardReadState state;
};

struct PostKnown {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	/** Where the document is, or empty where it is not held. */
	std::string path;
	/** Held, and known not to be the post it was asked for as. */
	bool failed = false;
};

struct PostAdopted {
	enum class Outcome {
		Verified,
		/** It is the post, but somebody other than the index's author wrote it. */
		Disputed,
		NoStore,
		Unreadable,
		TooLarge,
		WrongHash,
		NotAPost
	};

	BoardStoreHandle store = 0;
	uint64_t request = 0;
	Outcome outcome = Outcome::Unreadable;
	PostDocument post;
	/** Whether the store holds the document after this. */
	bool held = false;
};

struct PostsEvicted {
	BoardStoreHandle store = 0;
	uint64_t request = 0;
	int marked = 0;
};

/** OpenHub was given an address no session can be made for. */
struct HubOpenFailed { HubHandle hub = 0; };

struct HubNetStatus {
	HubHandle hub = 0;
	NetStatus status = NetStatus::Invalid;
	/** Whether the connection is encrypted, as of this change. */
	bool secure = false;
};

struct HubReconnectScheduled { HubHandle hub = 0; int seconds = 0; };
struct HubStatusChanged { HubHandle hub = 0; HubStatus status = HubStatus::None; };

/** The hub announced an extension; what that leaves this session able to do. */
struct HubExtensions { HubHandle hub = 0; bool richText = false; };

struct HubName { HubHandle hub = 0; std::string name; };
struct HubDescription { HubHandle hub = 0; std::string description; };
struct HubSystemError { HubHandle hub = 0; std::string message; };
struct HubAuthenticate { HubHandle hub = 0; };

/** 'followed' is what SetHubPolicy said, and so what the session did. */
struct HubRedirect {
	HubHandle hub = 0;
	std::string address;
	uint16_t port = 0;
	bool followed = false;
};

struct ChatReceived { HubHandle hub = 0; ChatLine line; };
struct HistoryRestored {
	HubHandle hub = 0;
	std::string conversation;
	std::vector<ChatLine> lines;
};

struct UserJoined { HubHandle hub = 0; std::string nick; };
struct UserLeft {
	HubHandle hub = 0;
	User user;
	std::string message;
	bool disconnected = false;
};
struct UserUpdated { HubHandle hub = 0; User user; };
struct UsersCleared { HubHandle hub = 0; };

struct SearchResult { HubHandle hub = 0; SearchHit hit; };

struct BoardsAvailable { HubHandle hub = 0; bool available = false; };
struct BoardDescribed { HubHandle hub = 0; Board board; };
struct BoardEntryArrived { HubHandle hub = 0; BoardEntry entry; };
struct BoardRefused {
	HubHandle hub = 0;
	int code = 0;
	std::string message;
	std::string board;
	std::string tth;
};

struct HubListFetched {
	std::string url;
	std::vector<HubListEntry> hubs;
};

/** 'reason' is a sentence fit to show, without the address in it. */
struct HubListFailed {
	std::string url;
	std::string reason;
};

struct FilelistReady {
	std::string hubUrl;
	std::string sourceId;
	RemoteDirectory root;
};

struct FilelistFailed {
	enum class Why {
		/** The download queue would not take the request. */
		Refused,
		/** It is this client's own list, and that could not be read. */
		OwnListUnreadable,
		/** The request ended without a list that could be read. */
		NotArrived
	};

	std::string hubUrl;
	std::string sourceId;
	Why why = Why::NotArrived;
};

/** A Search went out; 'hubsAsked' is how many hubs it went to. */
struct SearchSent {
	std::string token;
	int hubsAsked = 0;
};

struct TransferStarted { Transfer transfer; };
struct TransferFinished { Transfer transfer; };
struct TransferStopped { Transfer transfer; };
struct TransferError { Transfer transfer; };

/** Something was added to or left the download queue. */
struct QueueChanged { };

/** A download matched its hash and has been moved to 'filename'. */
struct DownloadCompleted {
	std::string filename;
	uint64_t size = 0;
	std::string tth;
};

struct TransfersSnapshot {
	uint64_t request = 0;
	std::vector<Transfer> active;
	std::vector<QueueItem> queued;
	/** Connections downloading for the queue right now. */
	uint32_t activeDownloads = 0;
};

/** A file offered by AttachFile is hashed and can now be asked for. */
struct AttachmentReady {
	std::string path;
	std::string tth;
	uint64_t size = 0;
};

struct AttachmentFailed {
	enum class Why {
		/** The core has nothing to offer files from. */
		NoShares,
		/** The path is not a file that can be read. */
		Unreadable,
		/** It was accepted, and then could not be offered; see 'reason'. */
		Failed
	};

	std::string path;
	Why why = Why::Failed;
	std::string reason;
};

struct ReachabilityCheckStarted {
	bool ipv6 = false;
};

struct ReachabilityMeasured {
	DropVerdict verdict;
};

struct ReachabilityCheckFailed {
	enum class Why {
		/** The core has no configuration to read a server from. */
		NoConfiguration,
		/** No reachability server is configured, so nobody can be asked. */
		NoServer,
		/** A run was made and did not complete; see 'reason'. */
		CheckFailed
	};

	Why why = Why::CheckFailed;
	std::string reason;
};

}

/*
 * Structs around the variants rather than aliases of them, so that a header
 * can name either type without including this one.
 */
struct Command {
	using Variant = std::variant<
		Commands::FetchHubList,
		Commands::MeasureReachability,
		Commands::LookupPublicAddress,
		Commands::ReplayReachability,
		Commands::ApplyPortForwarding,
		Commands::RepublishClientInfo,
		Commands::AttachFile,
		Commands::RequestFilelist,
		Commands::Search,
		Commands::Enqueue,
		Commands::EnqueueFromSearch,
		Commands::EnqueueForReading,
		Commands::AddFallbackSource,
		Commands::SetPriority,
		Commands::RemoveFromQueue,
		Commands::SnapshotTransfers,
		Commands::OpenHub,
		Commands::CloseHub,
		Commands::ConnectHub,
		Commands::DisconnectHub,
		Commands::SetHubPolicy,
		Commands::SendChat,
		Commands::SendPrivateChat,
		Commands::SendPassword,
		Commands::RequestHistory,
		Commands::SubscribeBoard,
		Commands::UnsubscribeBoard,
		Commands::RequestBoardEntry,
		Commands::SubmitPost,
		Commands::WithdrawPost,
		Commands::OpenBoardStore,
		Commands::CloseBoardStore,
		Commands::StoreBoardEntries,
		Commands::DescribeBoard,
		Commands::QueryBoardState,
		Commands::LoadBoard,
		Commands::ResumeBoard,
		Commands::MarkRead,
		Commands::ServePosts,
		Commands::QueryPost,
		Commands::AdoptPost,
		Commands::EvictPosts,
		Commands::ReleasePosts
	>;

	template<typename T>
	Command(T command) : value(std::move(command)) { }

	Variant value;
};

struct Event {
	using Variant = std::variant<
		Events::HubListFetched,
		Events::HubListFailed,
		Events::ReachabilityCheckStarted,
		Events::ReachabilityMeasured,
		Events::ReachabilityCheckFailed,
		Events::AttachmentReady,
		Events::AttachmentFailed,
		Events::FilelistReady,
		Events::FilelistFailed,
		Events::SearchSent,
		Events::TransferStarted,
		Events::TransferFinished,
		Events::TransferStopped,
		Events::TransferError,
		Events::QueueChanged,
		Events::DownloadCompleted,
		Events::TransfersSnapshot,
		Events::HubOpenFailed,
		Events::HubNetStatus,
		Events::HubReconnectScheduled,
		Events::HubStatusChanged,
		Events::HubExtensions,
		Events::HubName,
		Events::HubDescription,
		Events::HubSystemError,
		Events::HubAuthenticate,
		Events::HubRedirect,
		Events::ChatReceived,
		Events::HistoryRestored,
		Events::UserJoined,
		Events::UserLeft,
		Events::UserUpdated,
		Events::UsersCleared,
		Events::SearchResult,
		Events::BoardsAvailable,
		Events::BoardDescribed,
		Events::BoardEntryArrived,
		Events::BoardRefused,
		Events::BoardEntriesStored,
		Events::BoardStateReported,
		Events::BoardLoaded,
		Events::BoardResumed,
		Events::BoardReadChanged,
		Events::PostKnown,
		Events::PostAdopted,
		Events::PostsEvicted
	>;

	template<typename T>
	Event(T event) : value(std::move(event)) { }

	Event() = default;

	/** The event as 'T', or null where it is another. */
	template<typename T>
	const T* as() const { return std::get_if<T>(&value); }

	Variant value;
};

/** Told every event, on the core's thread, until its subscription ends. */
class EventSink {
	public:
		virtual ~EventSink() { }
		virtual void deliver(const Event& event) = 0;
};

}
}

#endif // HAVE_QUICKDC_API_PROTOCOL_H
