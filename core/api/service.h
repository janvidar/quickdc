/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_SERVICE_H
#define HAVE_QUICKDC_API_SERVICE_H

#include "api/protocol.h"
#include "api/subscription.h"

#include <map>
#include <memory>
#include <string>

namespace QuickDC {

class Core;

namespace Api {

/**
 * Where commands are carried out and events are published, on the core's thread.
 *
 * The one place a front end's request meets the core's objects, so that the
 * objects never have to be handed out: what the core lends a listener here is
 * copied into an Event before anybody outside sees it.
 */
class Service {
	public:
		explicit Service(Core& core);
		~Service();

		Service(const Service&) = delete;
		Service& operator=(const Service&) = delete;

		void execute(const Command& command);

		Subscription subscribe(EventSink* sink) { return sinks.subscribe(sink); }

		void publish(const Event& event);

	private:
		class HubListFetch;

		void run(const Commands::FetchHubList& command);

		Core& core;
		ListenerList<EventSink> sinks;
		std::map<std::string, std::unique_ptr<HubListFetch>> hublists;
};

}
}

#endif // HAVE_QUICKDC_API_SERVICE_H
