/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/service.h"
#include "api/core.h"
#include "api/dropverdict.h"
#include "api/networkmode.h"
#include "config/preferences.h"
#include "network/dropcheck.h"
#include "network/hublist.h"
#include "network/hubmanager.h"
#include "network/portforward.h"

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
	, dropObserved(observeDropCheck(core_, this))
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

bool QuickDC::Api::Service::haveReachabilityServer()
{
	using Failed = Events::ReachabilityCheckFailed;

	if (!core.config)
	{
		publish(Failed{Failed::Why::NoConfiguration, std::string()});
		return false;
	}

	if (core.config->peekString("Network", "Reachability server", "").empty())
	{
		publish(Failed{Failed::Why::NoServer, std::string()});
		return false;
	}

	return true;
}

/* A run refused because one is already going is not reported: that one will be. */
void QuickDC::Api::Service::run(const Commands::MeasureReachability&)
{
	if (!haveReachabilityServer()) return;

	startDropCheck(core, false);
}

void QuickDC::Api::Service::run(const Commands::LookupPublicAddress& command)
{
	if (!haveReachabilityServer()) return;

	startAddressLookup(core, command.ipv6);
}

void QuickDC::Api::Service::run(const Commands::ReplayReachability&)
{
	for (const bool ipv6 : {false, true})
	{
		const DropVerdict known = lastDropVerdict(core, ipv6);
		if (known.ran) publish(Events::ReachabilityMeasured{known});
	}
}

void QuickDC::Api::Service::run(const Commands::ApplyPortForwarding&)
{
	if (!core.portforward || !core.config) return;

	Preferences& config = *core.config;

	/*
	 * Derived rather than read off the key, because in automatic mode the stored
	 * UseUPnP is not what governs: the mode is. Every read peeks, since this runs
	 * on every save and a plain read would write UseUPnP into a file that had
	 * never mentioned it.
	 */
	const NetworkModeKeys keys = networkModeKeys(storedNetworkMode(config),
		config.peekBool("Network", "UseUPnP", false),
		config.peekBool("Network", "Force active mode", false),
		config.peekString("Network", "Reachability server", ""));

	const bool have = core.portforward->isForwarded() || core.portforward->isBusy();
	if (keys.useUpnp == have) return;

	if (keys.useUpnp)
		core.portforward->start(static_cast<uint16_t>(config.peekNumber("Network", "Port", 1411)));
	else
		core.portforward->stop();
}

void QuickDC::Api::Service::run(const Commands::RepublishClientInfo&)
{
	if (core.hubs) core.hubs->requestClientInfoUpdate();
}

void QuickDC::Api::Service::EventDropCheckStarted(bool ipv6)
{
	publish(Events::ReachabilityCheckStarted{ipv6});
}

void QuickDC::Api::Service::EventDropCheckFinished(const DropVerdict& verdict)
{
	publish(Events::ReachabilityMeasured{verdict});
}

void QuickDC::Api::Service::EventDropCheckFailed(const char* reason)
{
	using Failed = Events::ReachabilityCheckFailed;
	publish(Failed{Failed::Why::CheckFailed, reason ? reason : ""});
}
