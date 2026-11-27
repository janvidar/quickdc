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
#include "api/networkmode.h"

#include <cstdint>
#include <map>
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
	/** Whether the connection is encrypted. */
	bool secure = false;
	/** What the peer's certificate hashes to; empty where it presented none. */
	std::string peerKeyprint;
	/** Whether this end dialled the connection; false where the peer did. */
	bool outbound = false;
};

/** A folder offered to other users: what they see it called, and where it is here. */
struct ShareFolder {
	std::string name;
	std::string path;
};

/** A hub that was opened, and when it last was. */
struct RecentHub {
	std::string url;
	/** What the hub called itself, where it has said. */
	std::string name;
	/** Seconds since the epoch. */
	uint64_t visited = 0;
};

/** A download that finished and was verified. */
struct CompletedDownload {
	std::string filename;
	std::string tth;
	uint64_t size = 0;
	/** Seconds since the epoch. */
	uint64_t finished = 0;
};

/** A public hublist the directory reads, and how the last attempt at it went. */
struct HubListSource {
	std::string url;
	/** When the list in hand was fetched, in seconds since the epoch; 0 where there is none. */
	uint64_t fetched = 0;
	/** How many hubs the list in hand names. */
	uint64_t hubs = 0;
	/** Whether the list in hand was read from the cache rather than fetched by this run. */
	bool cached = false;
	bool fetching = false;
	/** Why the last attempt failed, fit to show; empty where it did not. */
	std::string error;
};

/** One hub of the merged directory, and which sources named it, as indices into its 'sources'. */
struct DirectoryHub {
	HubListEntry hub;
	std::vector<uint32_t> listedBy;
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

/** A connection the core holds: to a hub, to a peer, or one not yet told apart. */
struct PeerConnection {
	/** The kind, as the core names it: "ADC client", "ADCS hub", "NMDC client", "Probing", ... */
	std::string protocol;
	std::string address;
	uint16_t port = 0;
	/** A peer's CID on ADC or nickname on NMDC, or a hub's address; empty while not known. */
	std::string peer;
	bool secure = false;
	bool outbound = false;
};

/** What somebody on the core's own hub may do there. */
enum class LocalHubCredential { User, Operator, Admin };

struct LocalHubUser {
	enum class State { LoggingIn, Verifying, Normal, Leaving };

	std::string sid;
	/** Empty until the user has said who it is. */
	std::string nick;
	std::string cid;
	std::string address;
	LocalHubCredential credential = LocalHubCredential::User;
	State state = State::LoggingIn;
	/** Bytes waiting to be sent to this user. */
	uint64_t sendQueue = 0;
};

/** Somebody who logs in to the local hub with a password, and has these rights once they have. */
struct LocalHubAccount {
	std::string nick;
	LocalHubCredential credential = LocalHubCredential::User;
};

/** Who is kept off the local hub: by CID, nickname or address, whichever are not empty. */
struct LocalHubBan {
	/** What the ban was made for, which UnbanFromLocalHub takes too. */
	std::string name;
	std::string reason;
	/** Seconds since the epoch; 0 for good. */
	uint64_t until = 0;
	std::string cid;
	std::string nick;
	std::string address;
};

/**
 * A hub session as a front end names it: a number it picks when it opens one,
 * so two openings of the same address are two sessions, and an address typed
 * one way is never confused with the core's canonical spelling of it.
 */
using HubHandle = uint64_t;

/**
 * Handles from here up are the core's, for hubs it opens itself (LocalHubJoin);
 * a front end numbers its own below.
 */
inline constexpr HubHandle CORE_HUB_HANDLES = HubHandle(1) << 62;

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
	std::string name;
	std::string tth;
	uint64_t size = 0;
	uint32_t freeSlots = 0;
	User user;
};

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

/** The two preference files: the configuration, and what one session leaves the next. */
enum class SettingsStore { Application, Session };

/** Group, then key, then value; every value is text, as it is in the file. */
using SettingsGroups = std::map<std::string, std::map<std::string, std::string>>;

/** Both stores as they stood at one moment, with how many changes each had seen. */
struct SettingsSnapshot {
	SettingsGroups application;
	SettingsGroups session;
	uint64_t applicationRevision = 0;
	uint64_t sessionRevision = 0;
};

namespace Commands {

/**
 * Write one setting, or remove it. 'origin' is the writer's own number for the
 * write, which SettingsWritten echoes once it has been carried out.
 */
struct SetSetting {
	SettingsStore store = SettingsStore::Application;
	std::string group;
	std::string key;
	std::string value;
	bool remove = false;
	uint64_t origin = 0;
};

/** Remove 'removeKeys' from 'group' and set 'values' in it, as one write. */
struct ReplaceGroup {
	SettingsStore store = SettingsStore::Application;
	std::string group;
	std::vector<std::string> removeKeys;
	std::vector<std::pair<std::string, std::string>> values;
	uint64_t origin = 0;
};

/**
 * Write the store to its file now; answered by SettingsSaved. The core writes
 * a changed store by itself shortly after, so this is only for a confirmation.
 */
struct SaveSettings {
	SettingsStore store = SettingsStore::Application;
	uint64_t request = 0;
};

/**
 * Choose the connection mode, and have the core write every setting it decides.
 * 'useUpnp' and 'forceActive' are read in Advanced only, where they are the user's
 * to choose. Answered by ConnectionMode; SettingsWritten echoes 'origin'.
 */
struct SetConnectionMode {
	NetworkMode mode = NetworkMode::Advanced;
	bool useUpnp = false;
	bool forceActive = false;
	uint64_t origin = 0;
};

/**
 * Tell every piece of state a front end shows, each as the event that announces
 * its changes, with 'request' echoed; see Core::state().
 */
struct ReportState {
	uint64_t request = 0;
};

/*
 * Bulletin boards. The core keeps what every hub says about its boards under
 * the hub's address, follows each board a hub describes, and fetches, checks,
 * offers and lets go of post documents by [Boards] and each hub's own choices.
 * A board is named by its hub's address, however spelled, and its name, by every
 * command here: what needs the hub asks the session open on that address, and is
 * answered by BoardUnavailable (PostRefused for a post) where none carries boards.
 */

/** What is held of a board, read without asking its hub; told as BoardEntries (whole) and BoardState. */
struct LoadBoard {
	std::string hubUrl;
	std::string board;
};

/** Somebody opened a post: it is read, shown where its document is here, and fetched if 'fetch'. */
struct ReadPost {
	std::string hubUrl;
	std::string board;
	std::string tth;
	bool fetch = false;
};

/** Mark a post read, or with an empty 'tth' everything held of the board; told as BoardRead. */
struct MarkRead {
	std::string hubUrl;
	std::string board;
	std::string tth;
};

/**
 * Whether a hub fetches the rest of a thread when one post of it is fetched, or
 * offers the posts read from it: as [Boards] says, always, or never.
 */
struct SetBoardPolicy {
	enum class Policy { FetchAhead, Serve };
	enum class Choice { Follow, Always, Never };

	std::string hubUrl;
	Policy policy = Policy::FetchAhead;
	Choice choice = Choice::Follow;
};

/**
 * Follow a board again - one left, or every entry from the start where
 * 'fromStart' - rather than from the highest timestamp held.
 */
struct SubscribeBoard {
	std::string hubUrl;
	std::string board;
	bool fromStart = false;
};

/** Leave a board: the hub's descriptors do not take it up again while the session lasts. */
struct UnsubscribeBoard {
	std::string hubUrl;
	std::string board;
};

/** Ask the hub for one entry of a board, without following it. */
struct RequestBoardEntry {
	std::string hubUrl;
	std::string board;
	std::string tth;
};

/**
 * Write a post signed with this client's ID, offer its document and submit it;
 * answered by PostSubmitted or PostRefused. 'parent' is empty for a new thread.
 */
struct WritePost {
	uint64_t request = 0;
	std::string hubUrl;
	std::string board;
	std::string parent;
	std::string subject;
	std::string body;
	bool richText = false;
};

struct WithdrawPost {
	std::string hubUrl;
	std::string board;
	std::string tth;
};

/*
 * The hubs kept, the hubs visited, and what finished downloading. Addresses are
 * taken however they are spelled; each change is answered by the list it
 * changed - FavouriteHubs, RecentHubs, CompletedDownloads - and a change that
 * changes nothing by nothing.
 */

struct AddFavouriteHub { std::string url; };
struct RemoveFavouriteHub { std::string url; };

/** A hub is recorded as visited when it is opened; these forget visits. */
struct ForgetRecentHub { std::string url; };
struct ClearRecentHubs { };

/** Forget a finished download by the file it went to; the file is left alone. */
/** 'deleteFile' also deletes it from disk - only ever a file this list holds. */
struct ForgetCompletedDownload { std::string filename; bool deleteFile = false; };
struct ClearCompletedDownloads { };

/** Start a session for the hub at 'url', not yet connected; see ConnectHub. */
struct OpenHub {
	HubHandle hub = 0;
	std::string url;
	/** Kept among the hubs to open on the next start (SessionHubs) while open. */
	bool remember = true;
};

/** The front end is going away: hubs it closes from here on stay in SessionHubs. */
struct EndSession { };

/** Nothing to open on the next start but what is opened from now on. */
struct ClearSessionHubs { };

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


/*
 * The public hub directory: the sources it reads, in precedence order, each
 * change answered by HubListSources and, where the merged list changed with
 * it, HubDirectory. A source added is fetched unless its cache is fresh.
 */
struct AddHubListSource { std::string url; };
struct RemoveHubListSource { std::string url; };
struct ResetHubListSources { };

/**
 * Bring the directory up to date: fetch the sources whose list is stale, or
 * every one with 'force'. Cheap to send whenever the directory is looked at -
 * a source that was just asked, or whose list is fresh, is left alone.
 */
struct RefreshHubDirectory { bool force = false; };

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
 * Search every hub; answered by SearchSent with 'request' in it. The words are
 * matched as given, 'exclude' ones must not appear, and a non-empty 'extensions'
 * limits what is asked for to those. Results are told as the search's until
 * ForgetSearch.
 */
struct Search {
	enum class Size { Any, AtLeast, AtMost };

	uint64_t request = 0;
	std::vector<std::string> include;
	std::vector<std::string> exclude;
	std::vector<std::string> extensions;
	Size sizePolicy = Size::Any;
	uint64_t size = 0;
};

/** Tell no more results for 'search'; what arrives for it from now on is dropped. */
struct ForgetSearch { uint64_t search = 0; };

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
 * Fetch the file a message named by hash into the download folder, whatever the
 * picture policy says, looking for it on 'hub' alone where that is given; told
 * as ChatMedia, and offered again by its hash once it has arrived.
 */
struct FetchAttachment {
	HubHandle hub = 0;
	std::string tth;
	std::string name;
	uint64_t size = 0;
};

/**
 * Whether pictures in chat on the hub at 'hubUrl' are fetched by themselves:
 * always, never, or as the general setting says.
 */
struct SetChatMediaFetch {
	enum class Choice { Follow, Always, Never };

	std::string hubUrl;
	Choice choice = Choice::Follow;
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

/** Report what is shared as a ShareStatus, echoing 'request'. */
struct ReportShare {
	uint64_t request = 0;
};

/** Report the core itself as a CoreStatus, echoing 'request'. */
struct ReportStatus {
	uint64_t request = 0;
};

/** Report every connection the core holds as a ConnectionsSnapshot, echoing 'request'. */
struct SnapshotConnections {
	uint64_t request = 0;
};

/** Say whether a file with the hash 'tth' is offered from here, as a SharedFile echoing 'request'. */
struct FindShared {
	uint64_t request = 0;
	std::string tth;
};

/**
 * Have the hub disconnect the user 'userId', as one of its operators: told
 * 'message', sent to 'redirect' where that is not empty, and otherwise kept out
 * for 'banSeconds' (-1 for good) where that is not 0. What the hub then does is
 * told as for anybody leaving; KickRefused says where it was not asked.
 */
struct KickUser {
	HubHandle hub = 0;
	std::string userId;
	std::string message;
	std::string redirect;
	int64_t banSeconds = 0;
};

/*
 * The core's own hub. Each command is answered by a LocalHubState, or by a
 * LocalHubRefused, echoing 'request'.
 */

struct ReportLocalHub {
	uint64_t request = 0;
};

/** Disconnect whoever 'who' names on the local hub: a nickname, a CID or a SID. */
struct KickFromLocalHub {
	uint64_t request = 0;
	std::string who;
	std::string message;
};

/**
 * Keep 'who' off the local hub for 'seconds', for good where that is 0, and
 * disconnect whoever it names. Somebody on the hub is banned by CID and nickname;
 * otherwise an address is taken as an address, a CID as a CID and anything else
 * as a nickname.
 */
struct BanFromLocalHub {
	uint64_t request = 0;
	std::string who;
	std::string reason;
	int64_t seconds = 0;
};

/** Lift every ban 'who' names, by what it was made for or by any CID, nickname or address in it. */
struct UnbanFromLocalHub {
	uint64_t request = 0;
	std::string who;
};

/** An account for 'nick', replacing any it had; the hub asks whoever logs in as 'nick' for the password. */
struct SetLocalHubAccount {
	uint64_t request = 0;
	std::string nick;
	std::string password;
	LocalHubCredential credential = LocalHubCredential::User;
};

struct RemoveLocalHubAccount {
	uint64_t request = 0;
	std::string nick;
};

/*
 * Change the shared folders. Each is answered by ShareFolders and a ShareStatus,
 * or by ShareFolderRefused, echoing 'request'; a folder added is hashed in the
 * background, and ShareStatus says when it is done.
 */

/** An empty 'name' shows the folder by its own. */
struct AddShareFolder {
	std::string path;
	std::string name;
	uint64_t request = 0;
};

struct RemoveShareFolder {
	std::string path;
	uint64_t request = 0;
};

/** What others see the folder at 'path' called. */
struct RenameShareFolder {
	std::string path;
	std::string name;
	uint64_t request = 0;
};

/** Measure reachability against the configured server, IPv4 first. */
struct MeasureReachability { };

/**
 * Ask the channel's feed now rather than when the schedule would. Answered by
 * UpdateCurrent, UpdateReady (after UpdateAvailable, where it had to be fetched)
 * or UpdateFailed.
 */
struct CheckForUpdate { };

/**
 * The platform says the network changed; 'online' is whether anything beyond
 * it answers. The core also notices its own addresses changing, so this is
 * only what a front end knows that the core cannot see. Answered, once it has
 * settled and the host is online, by NetworkChanged.
 */
struct NoteNetworkChange { bool online = true; };

/** Ask the configured server only what public address it sees. */
struct LookupPublicAddress {
	bool ipv6 = false;
};

/** Say again every verdict already held, as ReachabilityMeasured. */
struct ReplayReachability { };

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

/**
 * Keep 'data', a pasted picture, among the core's media - a name made of when,
 * the extension 'suffix' - and offer it as AttachFile does. Answered by
 * PastedKept, then AttachmentReady or AttachmentFailed for the path. What is
 * kept goes once it is older than [Chat media] "Keep pasted for days".
 */
struct AttachPasted {
	uint64_t request = 0;
	std::string data;
	std::string suffix;
};

}

namespace Events {

/** One setting changed, whoever changed it; 'origin' is the write's, or 0. */
struct SettingChanged {
	SettingsStore store = SettingsStore::Application;
	std::string group;
	std::string key;
	bool removed = false;
	std::string value;
	/** The store's revision once this change is in it. */
	uint64_t revision = 0;
	uint64_t origin = 0;
};

/** The write 'origin' has been carried out, whether or not it changed anything. */
struct SettingsWritten {
	SettingsStore store = SettingsStore::Application;
	uint64_t origin = 0;
};

struct SettingsSaved {
	SettingsStore store = SettingsStore::Application;
	uint64_t request = 0;
	bool ok = false;
};

/**
 * The connection mode in force and what it decides, told on ReportState and,
 * with 'request' 0, whenever any of it changes.
 */
struct ConnectionMode {
	uint64_t request = 0;
	NetworkMode mode = NetworkMode::Advanced;
	/** Whether peers are told they may connect here. */
	bool activeMode = false;
	bool useUpnp = false;
	bool forceActive = false;
	/** Who a check would ask; the default where none is stored. */
	std::string reachabilityServer;
};

/**
 * What is held of a board: all of it, oldest first, where 'whole' (LoadBoard),
 * else what just arrived and was kept - a tombstone among them. 'read' names
 * those of 'entries' that read as read; 'unread' counts the whole board.
 */
struct BoardEntries {
	std::string hubUrl;
	std::string board;
	std::vector<BoardEntry> entries;
	bool whole = false;
	int unread = 0;
	std::vector<std::string> read;
};

/**
 * Where a board stands: what its hub last described it as, where 'described';
 * the cursor a subscription resumes from; whether what is held has a hole no
 * subscription will close; whether the session follows it, or somebody 'left'
 * it on this session; and 'gone' once its hub said it is no more. Told on
 * LoadBoard, a descriptor, a subscription or leaving one, and a refusal naming
 * the board as not there.
 */
struct BoardState {
	std::string hubUrl;
	std::string board;
	bool described = false;
	Board descriptor;
	uint64_t cursor = 0;
	BoardView view = BoardView::Unknown;
	bool subscribed = false;
	bool left = false;
	bool gone = false;
};

/** What reads as read moved: 'read' is every such post where 'whole', else the ones just marked. */
struct BoardRead {
	std::string hubUrl;
	std::string board;
	int unread = 0;
	std::vector<std::string> read;
	bool whole = false;
};

/** What is known of one post's document, wherever it is indexed. */
struct PostStatus {
	enum class State {
		/** Not here, and not being fetched. */
		Announced,
		Fetching,
		Verified,
		/** It is the post, but its document names an author other than the hub's. */
		Disputed,
		/** Not this time; it may be asked for again. */
		Unavailable,
		/** Here, and not the post it claims to be: every copy is these bytes. */
		Unreadable,
		Withdrawn
	};

	enum class Why {
		None,
		/** The hub named it with something that is not a hash. */
		NotAHash,
		Withdrawn,
		/** Declared larger than [Boards] lets one post be fetched; 'size' says how large. */
		TooLarge,
		/** Nothing bounds it: no size declared and no limit set. */
		Unsized,
		/** Its board has been asked for as much as it may be for the moment. */
		Allowance,
		/** Nobody who has it could be reached. */
		NobodyAnswered,
		/** The peer sent more than the post was said to be. */
		Oversent,
		/** What arrived is not the file that was asked for. */
		Mismatch,
		NoStore,
		/** The copy here could not be read. */
		Unreadable,
		/** Larger than this client will read for one post. */
		OverCap,
		/** Its bytes hash to another name, so they were thrown away. */
		WrongHash,
		/** Its header does not parse. */
		NotAPost,
		/** Known already not to be the post. */
		KnownBad,
		/** Let go of by the bounds on what is kept. */
		Evicted
	};

	std::string tth;
	State state = State::Announced;
	Why why = Why::None;
	uint64_t size = 0;
	/** Where Verified or Disputed. */
	PostDocument post;
	/** Who the post is attributed to: the CID the index entry named, never the document's. */
	std::string attributedTo;
};

/** WritePost was carried out: the document is offered and submitted under 'tth'. */
struct PostSubmitted {
	uint64_t request = 0;
	std::string hubUrl;
	std::string board;
	std::string tth;
	uint64_t size = 0;
};

struct PostRefused {
	enum class Why {
		/** This client has no ID to sign it with yet. */
		NoIdentity,
		/** A new thread needs a subject. */
		NoSubject,
		/** Larger than the board takes; 'size' and 'limit' say by how much. */
		TooLarge,
		/** The document could not be written; 'reason' says why. */
		NotWritten,
		/** The document could not be offered; 'reason' says why. */
		NotOffered,
		/** No session on that hub, or it does not carry boards. */
		NoHub
	};

	uint64_t request = 0;
	Why why = Why::NoHub;
	uint64_t size = 0;
	uint64_t limit = 0;
	std::string reason;
	std::string hubUrl;
	std::string board;
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

/** The host has settled on another network; reachability is measured again where it can be. */
struct NetworkChanged { };
struct HubAuthenticate { HubHandle hub = 0; };

/**
 * A hub sent the session elsewhere. 'address' is a host on NMDC, and on ADC the
 * address the hub gave, whole. Where 'followed', the session is connecting to
 * 'url' under the same handle and every later event names that hub; where not,
 * 'why' says why: SetHubPolicy said not to, the address is not one, it leads to
 * the other protocol or from adcs to adc, or to a hub this connection has
 * already been sent from or past QUICKDC_HUB_MAX_REDIRECTS of them.
 */
struct HubRedirect {
	enum class Why { None, Policy, Invalid, OtherProtocol, Downgrade, Loop };

	HubHandle hub = 0;
	std::string address;
	uint16_t port = 0;
	bool followed = false;
	std::string url;
	Why why = Why::None;
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

/** A result for the live search 'search'; one answering none is not told. */
struct SearchResult { HubHandle hub = 0; uint64_t search = 0; SearchHit hit; };

/** Whether the session open on hubUrl (as boards name it) carries boards now. */
struct BoardsAvailable { std::string hubUrl; bool available = false; };

/** No session on 'hubUrl' carries boards just now, so what was asked was not sent. */
struct BoardUnavailable {
	enum class Asked { Subscribe, Unsubscribe, Entry, Withdraw };

	std::string hubUrl;
	std::string board;
	std::string tth;
	Asked asked = Asked::Subscribe;
};
/** The hub on hubUrl declined a board request; it names the board or the post and nothing else. */
struct BoardRefused {
	std::string hubUrl;
	int code = 0;
	std::string message;
	std::string board;
	std::string tth;
};

/** The directory's sources and how each stands; told on ReportState and on every change. */
struct HubListSources {
	uint64_t request = 0;
	std::vector<HubListSource> sources;
};

/**
 * Every hub any source named, merged; told on ReportState and whenever a source's
 * list arrives, is read from the cache or goes. 'sources' are the addresses
 * 'listedBy' indexes.
 */
struct HubDirectory {
	uint64_t request = 0;
	std::vector<std::string> sources;
	std::vector<DirectoryHub> hubs;
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

/**
 * A Search went out as 'search', which its results name; 'hubsAsked' is how many
 * hubs it went to.
 */
struct SearchSent {
	uint64_t request = 0;
	uint64_t search = 0;
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

/** Where AttachPasted kept its picture; 'reason', fit to show, where it could not. */
struct PastedKept {
	uint64_t request = 0;
	std::string path;
	std::string reason;
};

/**
 * What became of a picture a rich text message showed, or of a file asked for
 * with FetchAttachment: queued to 'path', there at 'path' - already, or now -
 * or left for somebody to ask for, and why.
 */
struct ChatMedia {
	enum class State { Queued, Ready, Skipped };
	enum class Why {
		None,
		/** Pictures are not fetched by themselves on this hub. */
		Off,
		/** The message gave no size. */
		Unsized,
		/** Larger than [Chat media] "Maximum size". */
		TooLarge,
		/** The download queue would not take it. */
		Refused
	};

	HubHandle hub = 0;
	std::string tth;
	std::string name;
	uint64_t size = 0;
	State state = State::Queued;
	Why why = Why::None;
	std::string path;
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

/**
 * What is shared, told when ReportShare asks and each time hashing falls idle,
 * with 'request' 0; once 'hashing' is false every shared file can be searched
 * for and fetched by its hash.
 */
struct ShareStatus {
	uint64_t request = 0;
	uint64_t files = 0;
	uint64_t size = 0;
	bool hashing = false;
};

/** The shared folders, in order; told on ReportState and whenever they change. */
struct ShareFolders {
	uint64_t request = 0;
	std::vector<ShareFolder> folders;
};

/** The hubs kept, in the order they were added; told on ReportState and on every change. */
struct FavouriteHubs {
	uint64_t request = 0;
	std::vector<std::string> urls;
};

/** The hubs visited, newest first; told on ReportState and on every change. */
struct RecentHubs {
	uint64_t request = 0;
	std::vector<RecentHub> hubs;
};

/**
 * The hubs to open on the next start: those open now, and those closed by the
 * session ending (EndSession). Told on ReportState and on every change.
 */
struct SessionHubs {
	uint64_t request = 0;
	std::vector<std::string> urls;
};

/** What finished downloading, newest first; told on ReportState and on every change. */
struct CompletedDownloads {
	uint64_t request = 0;
	std::vector<CompletedDownload> downloads;
};

struct ShareFolderRefused {
	enum class Why {
		AlreadyShared,
		/** It is inside a shared folder, so everything in it is shared already. */
		InsideShared,
		NotADirectory,
		/** No shared folder has that path. */
		NotShared,
		/** Nothing is left of the name once what it cannot hold is taken out. */
		NoName
	};

	uint64_t request = 0;
	std::string path;
	Why why = Why::NotShared;
};

/** Why a KickUser was not sent to the hub. */
struct KickRefused {
	enum class Why {
		/** The session is not logged in. */
		NoSession,
		NoSuchUser,
		/** The hub has not made this session one of its operators. */
		NotOperator,
		/** The protocol cannot ask for a ban: NMDC. */
		Unsupported
	};

	HubHandle hub = 0;
	std::string userId;
	Why why = Why::NoSession;
};

/**
 * The core itself: what it is, since when, and what it listens on. Told on
 * ReportStatus and ReportState, and whenever what it listens on, or whether its
 * own hub takes logins, changes - so a front end can wait for the hub to be up.
 */
struct CoreStatus {
	uint64_t request = 0;
	std::string version;
	/** Seconds since the epoch. */
	uint64_t started = 0;
	/** The port peers and the local hub are reached on, as bound; 0 where nothing is. */
	uint16_t port = 0;
	/** The port asked for; not 'port' where that one was taken. */
	uint16_t configuredPort = 0;
	bool tcp = false;
	bool tcp6 = false;
	bool udp = false;
	bool udp6 = false;
	/** The core's own hub is on and takes logins on 'port'. */
	bool localHub = false;
};

struct ConnectionsSnapshot {
	uint64_t request = 0;
	std::vector<PeerConnection> connections;
};

/** The answer to FindShared; 'found' false and the rest empty where nothing is offered by that hash. */
struct SharedFile {
	uint64_t request = 0;
	std::string tth;
	bool found = false;
	/** Where it is on this disk. */
	std::string path;
	/** What peers see it as. */
	std::string name;
	uint64_t size = 0;
	/** Offered as an attachment rather than as part of a shared folder. */
	bool attached = false;
};

/** The core's own hub as it stands. */
struct LocalHubState {
	uint64_t request = 0;
	bool enabled = false;
	std::string name;
	uint32_t maxUsers = 0;
	/** Seconds since the epoch. */
	uint64_t started = 0;
	std::vector<LocalHubUser> users;
	std::vector<LocalHubAccount> accounts;
	std::vector<LocalHubBan> bans;
};

/**
 * The session the core opened to the hub it runs ([Local hub] Join), or
 * 'joined' 0 for none - not 'hub', since this is the core's news, not that hub's; a front end adopts it rather than opening its own. Told on
 * ReportState and on every change. A CloseHub of it holds until the hub is
 * turned off and on, or its port changes.
 */
struct LocalHubJoin {
	uint64_t request = 0;
	HubHandle joined = 0;
	std::string url;
};

struct LocalHubRefused {
	enum class Why {
		/** This core runs no hub. */
		NoHub,
		NoSuchUser,
		/** No ban, or no account, answers to what was named. */
		NoSuchEntry,
		/** Something the command needs is empty. */
		Invalid,
		/** The access list could not be written. */
		NotSaved
	};

	uint64_t request = 0;
	Why why = Why::Invalid;
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

/** Nothing newer than this build on its channel. */
struct UpdateCurrent {
	std::string version;
	/** What the feed offers, which is this or older. */
	std::string offered;
};

/** A newer version is on offer and is being fetched. */
struct UpdateAvailable {
	std::string version;
};

/** A newer version is downloaded and checked, and is installed on quit. */
struct UpdateReady {
	std::string version;
	/** The package, in the home's updates directory. */
	std::string path;
};

struct UpdateFailed {
	enum class Why {
		/** [Updates] Enabled is off. Said only when asked. */
		Disabled,
		/** This build has no channel, being built by hand. Said only when asked. */
		NoChannel,
		/** The channel has no key to check a feed with yet. */
		NoKey,
		/** This platform has no packages. */
		NoPlatform,
		/** The feed or the package could not be fetched. */
		Unreachable,
		/** The feed is malformed, or names something not to be fetched. */
		Invalid,
		/** The feed's signature does not verify. */
		BadSignature,
		/** A feed for another channel or platform than the one asked for. */
		WrongFeed,
		/** The package is not the size or the hash the feed says. */
		Integrity,
		/** The package could not be kept on disk. */
		Storage
	};

	Why why = Why::Unreachable;
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
		Commands::AddHubListSource,
		Commands::RemoveHubListSource,
		Commands::ResetHubListSources,
		Commands::RefreshHubDirectory,
		Commands::MeasureReachability,
		Commands::CheckForUpdate,
		Commands::NoteNetworkChange,
		Commands::LookupPublicAddress,
		Commands::ReplayReachability,
		Commands::AttachFile,
		Commands::AttachPasted,
		Commands::RequestFilelist,
		Commands::Search,
		Commands::ForgetSearch,
		Commands::Enqueue,
		Commands::EnqueueFromSearch,
		Commands::FetchAttachment,
		Commands::SetChatMediaFetch,
		Commands::SetPriority,
		Commands::RemoveFromQueue,
		Commands::SnapshotTransfers,
		Commands::ReportShare,
		Commands::ReportStatus,
		Commands::SnapshotConnections,
		Commands::FindShared,
		Commands::KickUser,
		Commands::ReportLocalHub,
		Commands::KickFromLocalHub,
		Commands::BanFromLocalHub,
		Commands::UnbanFromLocalHub,
		Commands::SetLocalHubAccount,
		Commands::RemoveLocalHubAccount,
		Commands::AddShareFolder,
		Commands::RemoveShareFolder,
		Commands::RenameShareFolder,
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
		Commands::WritePost,
		Commands::WithdrawPost,
		Commands::LoadBoard,
		Commands::ReadPost,
		Commands::MarkRead,
		Commands::SetBoardPolicy,
		Commands::SetSetting,
		Commands::ReplaceGroup,
		Commands::SaveSettings,
		Commands::SetConnectionMode,
		Commands::ReportState,
		Commands::AddFavouriteHub,
		Commands::RemoveFavouriteHub,
		Commands::ForgetRecentHub,
		Commands::ClearRecentHubs,
		Commands::EndSession,
		Commands::ClearSessionHubs,
		Commands::ForgetCompletedDownload,
		Commands::ClearCompletedDownloads
	>;

	template<typename T>
	Command(T command) : value(std::move(command)) { }

	Variant value;
};

struct Event {
	using Variant = std::variant<
		Events::HubListFetched,
		Events::HubListFailed,
		Events::HubListSources,
		Events::HubDirectory,
		Events::ReachabilityCheckStarted,
		Events::ReachabilityMeasured,
		Events::ReachabilityCheckFailed,
		Events::UpdateCurrent,
		Events::UpdateAvailable,
		Events::UpdateReady,
		Events::UpdateFailed,
		Events::AttachmentReady,
		Events::AttachmentFailed,
		Events::PastedKept,
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
		Events::ShareStatus,
		Events::ShareFolders,
		Events::ShareFolderRefused,
		Events::HubOpenFailed,
		Events::HubNetStatus,
		Events::HubReconnectScheduled,
		Events::HubStatusChanged,
		Events::HubExtensions,
		Events::HubName,
		Events::HubDescription,
		Events::HubSystemError,
		Events::NetworkChanged,
		Events::HubAuthenticate,
		Events::HubRedirect,
		Events::ChatReceived,
		Events::ChatMedia,
		Events::HistoryRestored,
		Events::UserJoined,
		Events::UserLeft,
		Events::UserUpdated,
		Events::UsersCleared,
		Events::SearchResult,
		Events::BoardsAvailable,
		Events::BoardRefused,
		Events::BoardUnavailable,
		Events::BoardEntries,
		Events::BoardState,
		Events::BoardRead,
		Events::PostStatus,
		Events::PostSubmitted,
		Events::PostRefused,
		Events::SettingChanged,
		Events::SettingsWritten,
		Events::SettingsSaved,
		Events::ConnectionMode,
		Events::FavouriteHubs,
		Events::RecentHubs,
		Events::SessionHubs,
		Events::LocalHubJoin,
		Events::CompletedDownloads,
		Events::KickRefused,
		Events::CoreStatus,
		Events::ConnectionsSnapshot,
		Events::SharedFile,
		Events::LocalHubState,
		Events::LocalHubRefused
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
