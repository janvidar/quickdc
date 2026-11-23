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
#include "api/hub.h"
#include "share/dcfilelist.h"
#include <samurai/io/file.h>
#include "network/hubmanager.h"
#include "network/portforward.h"
#include "share/sharemanager.h"

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

namespace {

QuickDC::Api::RemoteDirectory converted(const Share::RemoteDir* dir)
{
	QuickDC::Api::RemoteDirectory out;
	if (!dir) return out;

	out.name = dir->getName() ? dir->getName() : "";
	out.size = dir->getSize();
	out.fileCount = dir->getFiles();

	out.files.reserve(dir->countEntries());
	for (size_t n = 0; n < dir->countEntries(); ++n)
	{
		const Share::RemoteFile* file = dir->getEntry(n);
		if (!file) continue;

		QuickDC::Api::RemoteFile entry;
		entry.name = file->getName() ? file->getName() : "";
		entry.size = file->getSize();
		if (const char* tth = file->getTTH()) entry.tth = tth;
		out.files.push_back(std::move(entry));
	}

	out.dirs.reserve(dir->countSubdirs());
	for (size_t n = 0; n < dir->countSubdirs(); ++n)
		if (const Share::RemoteDir* subdir = dir->getSubdir(n))
			out.dirs.push_back(converted(subdir));

	return out;
}

/* The list at 'path', decoded; false where there is none or it cannot be read. */
bool decodeList(const std::string& path, QuickDC::Api::RemoteDirectory& root)
{
	Samurai::IO::File file(path);
	if (!file.exists()) return false;

	Share::RemoteFileList list(&file);
	if (!list.decode()) return false;

	root = converted(list.root);
	return true;
}

}

QuickDC::Api::Service::Service(Core& core_)
	: core(core_)
	, dropObserved(observeDropCheck(core_, this))
	, queueObserved(core_.queue ? core_.queue->subscribe(this) : Subscription())
{
}

QuickDC::Api::Service::~Service()
{
	/* An attachment still being hashed would otherwise report to this. */
	if (core.shares) core.shares->forgetListener(this);
}

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

void QuickDC::Api::Service::run(const Commands::AttachFile& command)
{
	using Failed = Events::AttachmentFailed;
	using Shares = Share::Manager;

	if (!core.shares)
	{
		publish(Failed{command.path, Failed::Why::NoShares, std::string()});
		return;
	}

	if (!core.shares->attach(command.path.c_str(), this,
	                         command.remembered ? Shares::Persistence::Remembered
	                                            : Shares::Persistence::Session,
	                         command.freeSlot ? Shares::SlotCost::Free
	                                          : Shares::SlotCost::Counted))
		publish(Failed{command.path, Failed::Why::Unreadable, std::string()});
}

void QuickDC::Api::Service::EventAttachmentReady(const char* path, const char* tth, uint64_t size)
{
	publish(Events::AttachmentReady{path ? path : "", tth ? tth : "", size});
}

void QuickDC::Api::Service::EventAttachmentFailed(const char* path, const char* reason)
{
	using Failed = Events::AttachmentFailed;
	publish(Failed{path ? path : "", Failed::Why::Failed, reason ? reason : ""});
}

void QuickDC::Api::Service::run(const Commands::RequestFilelist& command)
{
	using Failed = Events::FilelistFailed;

	Hub* hub = core.hubs ? core.hubs->getHubByUrl(command.hubUrl.c_str()) : nullptr;
	if (hub && hub->isLocalUser(command.sourceId.c_str()))
	{
		Events::FilelistReady ready{command.hubUrl, command.sourceId, RemoteDirectory()};
		bool decoded = false;

		if (core.shares)
		{
			const std::string path = core.shares->getFilelistDirectory() + "/files.xml";
			if (!Samurai::IO::File(path).exists()) core.shares->createFileLists(true);
			decoded = decodeList(path, ready.root);
		}

		if (decoded) publish(ready);
		else publish(Failed{command.hubUrl, command.sourceId, Failed::Why::OwnListUnreadable});
		return;
	}

	FilelistRequest* request = core.queue
		? core.queue->requestFilelist(command.hubUrl.c_str(), command.sourceId.c_str())
		: nullptr;

	if (!request)
	{
		publish(Failed{command.hubUrl, command.sourceId, Failed::Why::Refused});
		return;
	}

	/* Now rather than at the next turn, so the connection is asked for at once. */
	core.queue->process();
}

void QuickDC::Api::Service::EventFilelistFinished(const FilelistRequest* request)
{
	using Failed = Events::FilelistFailed;

	Events::FilelistReady ready{request->getHubUrl(), request->getClientID(), RemoteDirectory()};
	const char* path = request->getLocalFilename();

	if (path && decodeList(path, ready.root))
		publish(ready);
	else
		publish(Failed{ready.hubUrl, ready.sourceId, Failed::Why::NotArrived});
}
