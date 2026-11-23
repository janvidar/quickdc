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

namespace Commands {

/** Download and parse the hublist at 'url'; one at a time per address. */
struct FetchHubList {
	std::string url;
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
		Commands::AttachFile
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
		Events::AttachmentFailed
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
