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

namespace Commands {

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
		Commands::SnapshotTransfers
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
		Events::TransfersSnapshot
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
