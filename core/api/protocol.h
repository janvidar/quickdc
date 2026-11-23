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

#include "api/hublistentry.h"

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

}

/*
 * Structs around the variants rather than aliases of them, so that a header
 * can name either type without including this one.
 */
struct Command {
	using Variant = std::variant<
		Commands::FetchHubList
	>;

	template<typename T>
	Command(T command) : value(std::move(command)) { }

	Variant value;
};

struct Event {
	using Variant = std::variant<
		Events::HubListFetched,
		Events::HubListFailed
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
