/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_SERVICE_H
#define HAVE_QUICKDC_API_SERVICE_H

#include "api/attachments.h"
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
class Service : private DropCheckListener, private Share::AttachmentListener {
	public:
		explicit Service(Core& core);
		~Service() override;

		Service(const Service&) = delete;
		Service& operator=(const Service&) = delete;

		void execute(const Command& command);

		Subscription subscribe(EventSink* sink) { return sinks.subscribe(sink); }

		void publish(const Event& event);

	private:
		class HubListFetch;

		void run(const Commands::FetchHubList& command);
		void run(const Commands::MeasureReachability& command);
		void run(const Commands::LookupPublicAddress& command);
		void run(const Commands::ReplayReachability& command);
		void run(const Commands::ApplyPortForwarding& command);
		void run(const Commands::RepublishClientInfo& command);
		void run(const Commands::AttachFile& command);

		/** Whether there is a server to ask; publishes why not where there is not. */
		bool haveReachabilityServer();

		void EventDropCheckStarted(bool ipv6) override;
		void EventDropCheckFinished(const DropVerdict& verdict) override;
		void EventDropCheckFailed(const char* reason) override;

		void EventAttachmentReady(const char* path, const char* tth, uint64_t size) override;
		void EventAttachmentFailed(const char* path, const char* reason) override;

		Core& core;
		ListenerList<EventSink> sinks;
		Subscription dropObserved;
		std::map<std::string, std::unique_ptr<HubListFetch>> hublists;
};

}
}

#endif // HAVE_QUICKDC_API_SERVICE_H
