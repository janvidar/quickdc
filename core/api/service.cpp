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
#include "share/searchrequest.h"
#include "network/filesource.h"
#include "network/transfer.h"
#include <samurai/io/file.h>
#include "network/hubmanager.h"
#include "network/portforward.h"
#include "share/sharemanager.h"

#include <samurai/io/net/url.h>

#include <string_view>
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

static_assert(std::string_view(QuickDC::Api::PART_SUFFIX) == QUICKDC_PART_SUFFIX,
              "the protocol names the part suffix the queue writes");

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

QuickDC::Api::Priority converted(QuickDC::QueueFile::Priority priority)
{
	switch (priority)
	{
		case QuickDC::QueueFile::PrioPaused: return QuickDC::Api::Priority::Paused;
		case QuickDC::QueueFile::PrioLow:    return QuickDC::Api::Priority::Low;
		case QuickDC::QueueFile::PrioNormal: return QuickDC::Api::Priority::Normal;
		case QuickDC::QueueFile::PrioHigh:   return QuickDC::Api::Priority::High;
	}
	return QuickDC::Api::Priority::Normal;
}

QuickDC::QueueFile::Priority converted(QuickDC::Api::Priority priority)
{
	switch (priority)
	{
		case QuickDC::Api::Priority::Paused: return QuickDC::QueueFile::PrioPaused;
		case QuickDC::Api::Priority::Low:    return QuickDC::QueueFile::PrioLow;
		case QuickDC::Api::Priority::Normal: return QuickDC::QueueFile::PrioNormal;
		case QuickDC::Api::Priority::High:   return QuickDC::QueueFile::PrioHigh;
	}
	return QuickDC::QueueFile::PrioNormal;
}

QuickDC::Api::Transfer converted(QuickDC::Transfer* transfer)
{
	QuickDC::Api::Transfer out;
	if (!transfer) return out;

	out.id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(transfer));
	if (Samurai::IO::File* file = transfer->getFile()) out.filename = file->getName();
	out.transferred = transfer->getTransferred();
	out.length = transfer->getLength();
	out.bytesPerSecond = transfer->getBps();
	out.ratio = transfer->getRatio();
	out.upload = transfer->isUpload();
	out.finished = transfer->isFinished();
	return out;
}

QuickDC::Api::QueueItem converted(const QuickDC::QueueStatus& status)
{
	QuickDC::Api::QueueItem out;
	out.filename = status.filename ? status.filename : "";
	out.tth = status.tth ? status.tth : "";
	out.size = status.size;
	out.done = status.done;
	out.sources = static_cast<uint32_t>(status.sources);
	out.activeSources = static_cast<uint32_t>(status.activeSources);
	out.priority = converted(status.priority);
	out.complete = status.complete;
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
	, transfersObserved(core_.transfers ? core_.transfers->subscribe(this) : Subscription())
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

void QuickDC::Api::Service::run(const Commands::Search& command)
{
	if (!core.hubs) return;

	using Request = Share::SearchRequest;
	Request::SizePolicy policy = Request::SizeAny;
	if (command.sizePolicy == Commands::Search::Size::AtLeast) policy = Request::SizeMin;
	if (command.sizePolicy == Commands::Search::Size::AtMost) policy = Request::SizeMax;

	Request request(policy, command.size, nullptr, command.token.c_str(), nullptr);
	for (const std::string& term : command.include) request.addInclusion(term.c_str());
	for (const std::string& term : command.exclude) request.addExclusion(term.c_str());
	for (const std::string& ext : command.extensions) request.addExtension(ext.c_str());

	const int asked = static_cast<int>(core.hubs->searchAllHubs(&request));
	publish(Events::SearchSent{command.token, asked});
}

void QuickDC::Api::Service::run(const Commands::Enqueue& command)
{
	if (!core.queue) return;

	core.queue->add(command.filename.c_str(),
	                command.tth.empty() ? nullptr : command.tth.c_str(), command.size);
}

void QuickDC::Api::Service::run(const Commands::EnqueueFromSearch& command)
{
	if (!core.queue || command.hubUrl.empty() || command.sourceId.empty()) return;

	QueueFile* file = core.queue->add(command.filename.c_str(),
		command.tth.empty() ? nullptr : command.tth.c_str(), command.size);
	if (!file) return;

	if (file->findSource(command.hubUrl.c_str(), command.sourceId.c_str())) return;

	file->addSource(new FileSource(command.hubUrl.c_str(), command.sourceId.c_str(),
		command.remoteName.empty() ? nullptr : command.remoteName.c_str()));
}

void QuickDC::Api::Service::run(const Commands::EnqueueForReading& command)
{
	if (!core.queue || command.tth.empty()) return;

	QueueFile* file = core.queue->add(command.filename.c_str(), command.tth.c_str(), command.size);
	if (file && !command.onlyHub.empty()) file->searchOnlyOn(command.onlyHub.c_str());
	core.queue->requestSourceSearch(command.tth.c_str());
}

/* Only where nothing else is known: a source found by searching is a better one. */
void QuickDC::Api::Service::run(const Commands::AddFallbackSource& command)
{
	if (!core.queue) return;

	QueueFile* file = core.queue->getFileByTTH(command.tth.c_str());
	if (!file || file->countSources() > 0) return;

	core.queue->addKnownSource(command.tth.c_str(), command.hubUrl.c_str(),
	                           command.sourceId.c_str());
}

void QuickDC::Api::Service::run(const Commands::SetPriority& command)
{
	if (!core.queue) return;

	if (QueueFile* file = core.queue->getFileByName(command.filename.c_str()))
		file->setPriority(converted(command.priority));

	publish(Events::QueueChanged{});
}

void QuickDC::Api::Service::run(const Commands::RemoveFromQueue& command)
{
	if (!core.queue) return;

	QueueFile* file = core.queue->getFileByName(command.filename.c_str());
	if (!file) return;

	/* Copied first: the entry, and its idea of where the part is, go with remove(). */
	std::string partial;
	if (command.deletePartial && file->getTempFilename()) partial = file->getTempFilename();

	core.queue->remove(file);
	if (!partial.empty()) Samurai::IO::File(partial).remove();
}

void QuickDC::Api::Service::run(const Commands::SnapshotTransfers& command)
{
	Events::TransfersSnapshot taken;
	taken.request = command.request;

	if (core.transfers)
		for (QuickDC::Transfer* t = core.transfers->first(); t; t = core.transfers->next())
			taken.active.push_back(converted(t));

	if (core.queue)
	{
		std::vector<QueueStatus> status;
		core.queue->getStatus(status);
		taken.queued.reserve(status.size());
		for (const QueueStatus& entry : status) taken.queued.push_back(converted(entry));
		taken.activeDownloads = static_cast<uint32_t>(core.queue->countActiveTransfers());
	}

	publish(taken);
}

void QuickDC::Api::Service::EventQueueFileAdded(const QueueFile*)
{
	publish(Events::QueueChanged{});
}

void QuickDC::Api::Service::EventQueueFileRemoved(const QueueFile*)
{
	publish(Events::QueueChanged{});
}

void QuickDC::Api::Service::EventQueueFileVerified(const QueueFile* file)
{
	if (!file) return;

	publish(Events::DownloadCompleted{
		file->getLocalFilename() ? file->getLocalFilename() : "",
		file->getSize(),
		file->getTTH() ? file->getTTH() : ""});
}

void QuickDC::Api::Service::EventTransferStarted(QuickDC::Transfer* transfer)
{
	publish(Events::TransferStarted{converted(transfer)});
}

void QuickDC::Api::Service::EventTransferFinished(QuickDC::Transfer* transfer)
{
	publish(Events::TransferFinished{converted(transfer)});
}

void QuickDC::Api::Service::EventTransferStopped(QuickDC::Transfer* transfer)
{
	publish(Events::TransferStopped{converted(transfer)});
}

void QuickDC::Api::Service::EventTransferError(QuickDC::Transfer* transfer)
{
	publish(Events::TransferError{converted(transfer)});
}
