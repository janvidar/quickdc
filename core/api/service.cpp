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
#include "chat/archive.h"
#include "network/user.h"
#include "share/searchresult.h"
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

namespace {

std::string text(const char* value)
{
	return value ? value : "";
}

QuickDC::Api::User converted(const QuickDC::User* user)
{
	QuickDC::Api::User out;
	if (!user) return out;

	out.id          = text(user->getID());
	out.nick        = text(user->getNick());
	out.description = text(user->getDescription());
	out.email       = text(user->getEmail());
	out.speed       = text(user->getSpeed());
	out.userAgent   = text(user->getUserAgent());
	out.addressIPv4 = text(user->getAddressIPv4());
	out.addressIPv6 = text(user->getAddressIPv6());
	out.keyprint    = text(user->getKeyprint());

	/* An NMDC user has no client ID, and a source names them by nickname. */
	const char* cid = reinterpret_cast<const char*>(user->getClientID());
	out.sourceId    = (cid && *cid) ? std::string(cid) : out.nick;

	out.sharedBytes = user->getSharedBytes();
	out.sharedFiles = user->getSharedFiles();
	out.slotCount   = user->getNumSlots();
	out.op          = user->isOp();
	out.away        = user->isAway();
	return out;
}

QuickDC::Api::NetStatus converted(QuickDC::HubListener::StatusNetwork status)
{
	using In = QuickDC::HubListener;
	using Out = QuickDC::Api::NetStatus;

	switch (status)
	{
		case In::NetInvalid:                return Out::Invalid;
		case In::NetLookup:                 return Out::Lookup;
		case In::NetConnecting:             return Out::Connecting;
		case In::NetConnected:              return Out::Connected;
		case In::NetDisconnected:           return Out::Disconnected;
		case In::NetTlsConnected:           return Out::TlsConnected;
		case In::NetTlsDisconnected:        return Out::TlsDisconnected;
		case In::NetErrorTimeout:           return Out::ErrorTimeout;
		case In::NetErrorConnectionRefused: return Out::ErrorConnectionRefused;
		case In::NetErrorHostNotFound:      return Out::ErrorHostNotFound;
		case In::NetErrorHostUnreachable:   return Out::ErrorHostUnreachable;
		case In::NetErrorIO:                return Out::ErrorIO;
		case In::NetTlsError:               return Out::TlsError;
	}
	return Out::Invalid;
}

QuickDC::Api::HubStatus converted(QuickDC::HubListener::StatusHub status)
{
	using In = QuickDC::HubListener;
	using Out = QuickDC::Api::HubStatus;

	switch (status)
	{
		case In::HubNone:                 return Out::None;
		case In::HubLogin:                return Out::Login;
		case In::HubPassword:             return Out::Password;
		case In::HubLoggedIn:             return Out::LoggedIn;
		case In::HubErrorLoginRefused:    return Out::ErrorLoginRefused;
		case In::HubErrorFull:            return Out::ErrorFull;
		case In::HubErrorDisabled:        return Out::ErrorDisabled;
		case In::HubErrorNickNotAccepted: return Out::ErrorNickNotAccepted;
		case In::HubErrorNickTaken:       return Out::ErrorNickTaken;
		case In::HubErrorCIDTaken:        return Out::ErrorCidTaken;
		case In::HubErrorWrongPassword:   return Out::ErrorWrongPassword;
		case In::HubKicked:               return Out::Kicked;
		case In::HubBannedTemporarily:    return Out::BannedTemporarily;
		case In::HubBannedPermanently:    return Out::BannedPermanently;
		case In::HubRedirected:           return Out::Redirected;
	}
	return Out::None;
}

}

/**
 * One hub session, reporting everything it hears as an event for its handle.
 *
 * What the core lends a listener - a User it deletes on the next line, a string
 * in a buffer the next command reuses - is copied here, before publish().
 */
class QuickDC::Api::Service::HubSession final : public QuickDC::Hub, public QuickDC::HubListener {
	public:
		HubSession(Service& owner, HubHandle handle, Samurai::IO::Net::URL* url)
			: QuickDC::Hub(owner.core, this, url)
			, owner(owner)
			, handle(handle)
		{
		}

		void setPolicy(bool follow, bool allowPeers)
		{
			followRedirects = follow;
			allowPeerConnections = allowPeers;
		}

		void sendPrivateChat(const std::string& userId, const std::string& message, bool richText)
		{
			const QuickDC::User* user = session ? session->getUser(userId.c_str()) : nullptr;
			if (!user)
			{
				EventSystemError("sendPrivateChat(): No such user");
				return;
			}
			sendPrivateChatMessage(user, message.c_str(), richText);
		}

		void EventNetStatus(enum StatusNetwork netstate) override
		{
			owner.publish(Events::HubNetStatus{handle, converted(netstate), isSecure()});
		}

		void EventReconnectScheduled(int seconds) override
		{
			owner.publish(Events::HubReconnectScheduled{handle, seconds});
		}

		void EventHubStatus(enum StatusHub hubstate) override
		{
			owner.publish(Events::HubStatusChanged{handle, converted(hubstate)});
		}

		void EventHubExtension(const char*) override
		{
			owner.publish(Events::HubExtensions{handle, supportsRichText()});
		}

		void EventBoardsAvailable(bool available) override
		{
			owner.publish(Events::BoardsAvailable{handle, available});
		}

		void EventBoardDescribed(const QuickDC::Board& board) override
		{
			owner.publish(Events::BoardDescribed{handle, board});
		}

		void EventBoardEntry(const QuickDC::BoardEntry& entry) override
		{
			owner.publish(Events::BoardEntryArrived{handle, entry});
		}

		void EventBoardRefused(int code, const char* message, const char* board,
		                       const char* tth) override
		{
			owner.publish(Events::BoardRefused{handle, code, text(message), text(board), text(tth)});
		}

		void EventSystemError(const char* msg) override
		{
			owner.publish(Events::HubSystemError{handle, text(msg)});
		}

		void EventChat(const QuickDC::User* user, const char* message, bool action,
		               bool richText, uint64_t timestamp) override
		{
			ChatLine line;
			line.hubUrl = text(getUrl());
			line.from = converted(user);
			line.text = text(message);
			line.timestamp = timestamp;
			line.action = action;
			line.richText = richText;
			owner.publish(Events::ChatReceived{handle, std::move(line)});
		}

		void EventPrivateChat(const QuickDC::User* from, const QuickDC::User*,
		                      const char* message, const char* userContextID,
		                      bool action, bool richText, uint64_t timestamp) override
		{
			ChatLine line;
			line.hubUrl = text(getUrl());
			line.from = converted(from);
			line.text = text(message);
			line.context = text(userContextID);
			line.timestamp = timestamp;
			line.action = action;
			line.richText = richText;
			line.privateMessage = true;

			/* A conversation with nobody else in it is named after who it is with. */
			if (line.context.empty()) line.context = line.from.id;

			owner.publish(Events::ChatReceived{handle, std::move(line)});
		}

		void EventHubMessage(const char* fakeuser, const char* message, bool action,
		                     bool richText, uint64_t timestamp) override
		{
			ChatLine line;
			line.hubUrl = text(getUrl());
			line.fromHub = text(fakeuser);
			line.text = text(message);
			line.timestamp = timestamp;
			line.action = action;
			line.richText = richText;
			owner.publish(Events::ChatReceived{handle, std::move(line)});
		}

		void EventUserJoin(const char* user) override
		{
			owner.publish(Events::UserJoined{handle, text(user)});
		}

		void EventUserLeave(const QuickDC::User* user, const char* message,
		                    bool disconnect) override
		{
			owner.publish(Events::UserLeft{handle, converted(user), text(message), disconnect});
		}

		void EventUsersCleanup() override
		{
			owner.publish(Events::UsersCleared{handle});
		}

		void EventUserUpdate(const QuickDC::User* user) override
		{
			owner.publish(Events::UserUpdated{handle, converted(user)});
		}

		void EventSearchResult(void* pointer) override
		{
			const auto* result = static_cast<const QuickDC::Share::SearchResult*>(pointer);
			if (!result) return;

			SearchHit hit;
			hit.hubUrl = text(getUrl());
			hit.token = text(result->getToken());
			hit.name = text(result->getFile());
			hit.tth = text(result->getTTH());
			hit.size = result->getSize();
			hit.freeSlots = static_cast<uint32_t>(result->getFreeSlots());
			hit.user = converted(result->getUser());
			owner.publish(Events::SearchResult{handle, std::move(hit)});
		}

		void EventHubName(const char* hubname) override
		{
			owner.publish(Events::HubName{handle, text(hubname)});
		}

		void EventHubDescription(const char* description) override
		{
			owner.publish(Events::HubDescription{handle, text(description)});
		}

		bool EventHubRedirect(const char* address, uint16_t port) override
		{
			owner.publish(Events::HubRedirect{handle, text(address), port, followRedirects});
			return followRedirects;
		}

		void EventHubAutenticate() override
		{
			owner.publish(Events::HubAuthenticate{handle});
		}

		bool EventClientConnect(const char*, uint16_t, const QuickDC::User*) override
		{
			return allowPeerConnections;
		}

		bool EventClientConnect(const QuickDC::User*) override
		{
			return allowPeerConnections;
		}

	private:
		Service& owner;
		HubHandle handle;
		bool followRedirects = false;
		bool allowPeerConnections = true;
};

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

QuickDC::Api::Service::HubSession* QuickDC::Api::Service::session(HubHandle hub)
{
	const auto found = hubs.find(hub);
	return found == hubs.end() ? nullptr : found->second.get();
}

void QuickDC::Api::Service::run(const Commands::OpenHub& command)
{
	/* Hub copies the address it is given. */
	Samurai::IO::Net::URL parsed(command.url);
	if (!parsed.isValid() || hubs.count(command.hub))
	{
		publish(Events::HubOpenFailed{command.hub});
		return;
	}

	hubs[command.hub] = std::make_unique<HubSession>(*this, command.hub, &parsed);
}

/* Disconnected first: ~Hub frees the session, and does not close it. */
void QuickDC::Api::Service::run(const Commands::CloseHub& command)
{
	const auto found = hubs.find(command.hub);
	if (found == hubs.end()) return;

	std::unique_ptr<HubSession> closing = std::move(found->second);
	hubs.erase(found);
	closing->disconnect();
}

void QuickDC::Api::Service::run(const Commands::ConnectHub& command)
{
	if (HubSession* hub = session(command.hub)) hub->connect();
}

void QuickDC::Api::Service::run(const Commands::DisconnectHub& command)
{
	if (HubSession* hub = session(command.hub)) hub->disconnect();
}

void QuickDC::Api::Service::run(const Commands::SetHubPolicy& command)
{
	if (HubSession* hub = session(command.hub))
		hub->setPolicy(command.followRedirects, command.allowPeerConnections);
}

void QuickDC::Api::Service::run(const Commands::SendChat& command)
{
	if (HubSession* hub = session(command.hub))
		hub->sendChatMessage(command.text.c_str(), command.richText);
}

void QuickDC::Api::Service::run(const Commands::SendPrivateChat& command)
{
	if (HubSession* hub = session(command.hub))
		hub->sendPrivateChat(command.userId, command.text, command.richText);
}

void QuickDC::Api::Service::run(const Commands::SendPassword& command)
{
	if (HubSession* hub = session(command.hub)) hub->sendPassword(command.password.c_str());
}

void QuickDC::Api::Service::run(const Commands::RequestHistory& command)
{
	Events::HistoryRestored restored{command.hub, command.conversation, {}};

	if (core.chat && command.limit > 0)
	{
		const std::vector<ChatArchive::Line> found = core.chat->tail(
			command.hubUrl.c_str(), command.conversation.c_str(),
			static_cast<size_t>(command.limit));

		restored.lines.reserve(found.size());
		for (const ChatArchive::Line& stored : found)
		{
			ChatLine line;
			line.hubUrl = command.hubUrl;
			line.text = stored.text;
			line.fromHub = stored.hub;
			line.from.nick = stored.nick;
			line.from.sourceId = stored.source;
			line.from.op = stored.op;
			line.action = stored.action;
			line.richText = stored.richText;
			line.privateMessage = stored.privateMessage;
			line.timestamp = stored.time;
			restored.lines.push_back(std::move(line));
		}
	}

	publish(restored);
}

void QuickDC::Api::Service::run(const Commands::SubscribeBoard& command)
{
	if (HubSession* hub = session(command.hub))
		hub->subscribeBoard(command.board.c_str(), command.timestamp);
}

void QuickDC::Api::Service::run(const Commands::UnsubscribeBoard& command)
{
	if (HubSession* hub = session(command.hub)) hub->unsubscribeBoard(command.board.c_str());
}

void QuickDC::Api::Service::run(const Commands::RequestBoardEntry& command)
{
	if (HubSession* hub = session(command.hub))
		hub->requestEntry(command.board.c_str(), command.tth.c_str());
}

void QuickDC::Api::Service::run(const Commands::SubmitPost& command)
{
	HubSession* hub = session(command.hub);
	if (!hub) return;

	hub->submitPost(command.tth.c_str(), command.size, command.board.c_str(),
	                command.subject.empty() ? nullptr : command.subject.c_str(),
	                command.parent.empty() ? nullptr : command.parent.c_str());
}

void QuickDC::Api::Service::run(const Commands::WithdrawPost& command)
{
	if (HubSession* hub = session(command.hub))
		hub->withdrawPost(command.tth.c_str(), command.board.c_str());
}
