/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/service.h"
#include "api/core.h"
#include "network/hublist.h"

#include <samurai/io/net/url.h>

#include <utility>

/** One download, kept for as long as the service so a second fetch reuses it. */
class QuickDC::Api::Service::HubListFetch final : public QuickDC::HubListListener {
	public:
		HubListFetch(Service& owner, std::string url)
			: owner(owner)
			, url(std::move(url))
			, list(this)
		{
		}

		void start()
		{
			const Samurai::IO::Net::URL parsed(url);
			if (!parsed.isValid())
			{
				EventHubListFailure("it is not an address a hublist can be fetched from");
				return;
			}

			list.download(parsed);
		}

		void EventHubList(const std::vector<QuickDC::HubListEntry>& hubs) override
		{
			owner.publish(Events::HubListFetched{url, hubs});
		}

		void EventHubListFailure(const char* reason) override
		{
			owner.publish(Events::HubListFailed{url, reason ? reason : ""});
		}

	private:
		Service& owner;
		std::string url;
		QuickDC::HubList list;
};

QuickDC::Api::Service::Service(Core& core_)
	: core(core_)
{
}

QuickDC::Api::Service::~Service() = default;

void QuickDC::Api::Service::execute(const Command& command)
{
	std::visit([this](const auto& c) { run(c); }, command.value);
}

void QuickDC::Api::Service::publish(const Event& event)
{
	sinks.notify([&event](EventSink& sink) { sink.deliver(event); });
}

void QuickDC::Api::Service::run(const Commands::FetchHubList& command)
{
	std::unique_ptr<HubListFetch>& fetch = hublists[command.url];
	if (!fetch) fetch = std::make_unique<HubListFetch>(*this, command.url);
	fetch->start();
}
