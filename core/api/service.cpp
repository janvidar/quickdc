/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/service.h"
#include "config/settingskeeper.h"
#include "api/core.h"
#include "api/dropverdict.h"
#include "api/networkmode.h"
#include "config/preferences.h"
#include "config/settings.h"
#include "network/dropcheck.h"
#include "network/hublist.h"
#include "api/hub.h"
#include "chat/archive.h"
#include "chat/chatmedia.h"
#include "api/huburl.h"
#include "network/user.h"
#include "share/searchresult.h"
#include "share/dcfilelist.h"
#include "share/searchrequest.h"
#include "network/filesource.h"
#include "network/transfer.h"
#include "network/keyprint.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/file.h>
#include "network/hubmanager.h"
#include "quickdc.h"
#include <samurai/messagehandler.h>
#include "network/portforward.h"
#include "share/sharemanager.h"
#include "hash/hashmanager.h"
#include "network/connection.h"
#include "network/connectionmanager.h"
#include "network/adc/cid.h"
#include "network/adc/server/acl.h"
#include "network/adc/server/hub.h"
#include "network/adc/server/user.h"
#include "api/server.h"
#include <samurai/io/net/inetaddress.h>

#include <samurai/io/net/url.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

/** One download, kept for as long as the service so a second fetch reuses it. */
class QuickDC::Api::Service::HubListFetch final : public QuickDC::HubListListener {
	public:
		HubListFetch(Service& owner, std::string url)
			: owner(owner)
			, url(std::move(url))
			, list(owner.core.loop(), this)
		{
		}

		bool busy() const { return list.isBusy(); }

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

	if (Samurai::IO::Net::Socket* socket = transfer->getSocket())
	{
		out.secure = socket->isSecure();
		if (out.secure && !QuickDC::Keyprint::ofPeer(socket, out.peerKeyprint)) out.peerKeyprint.clear();
		out.outbound = socket->isOutbound();
	}
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

QuickDC::Api::Events::HubRedirect::Why converted(QuickDC::RedirectRefusal why)
{
	using In = QuickDC::RedirectRefusal;
	using Out = QuickDC::Api::Events::HubRedirect::Why;

	switch (why)
	{
		case In::None:          return Out::None;
		case In::Policy:        return Out::Policy;
		case In::Invalid:       return Out::Invalid;
		case In::OtherProtocol: return Out::OtherProtocol;
		case In::Downgrade:     return Out::Downgrade;
		case In::Loop:          return Out::Loop;
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
class QuickDC::Api::Service::HubSession final : public QuickDC::Hub, public QuickDC::HubListener,
                                                 public QuickDC::BoardKeeper::Link {
	public:
		HubSession(Service& owner, HubHandle handle, Samurai::IO::Net::URL* url, std::string opened)
			: QuickDC::Hub(owner.core, this, url)
			, owner(owner)
			, handle(handle)
			, opened(std::move(opened))
		{
		}

		void setPolicy(bool follow, bool allowPeers)
		{
			setFollowRedirects(follow);
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

		bool subscribeBoard(const std::string& board, uint64_t cursor) override
		{
			return Hub::subscribeBoard(board.c_str(), cursor);
		}

		bool unsubscribeBoard(const std::string& board) override
		{
			return Hub::unsubscribeBoard(board.c_str());
		}

		bool submitPost(const std::string& tth, uint64_t size, const std::string& board,
		                const std::string& subject, const std::string& parent) override
		{
			return Hub::submitPost(tth.c_str(), size, board.c_str(),
			                       subject.empty() ? nullptr : subject.c_str(),
			                       parent.empty() ? nullptr : parent.c_str());
		}

		bool requestEntry(const std::string& board, const std::string& tth) override
		{
			return Hub::requestEntry(board.c_str(), tth.c_str());
		}

		bool withdrawPost(const std::string& board, const std::string& tth) override
		{
			return Hub::withdrawPost(tth.c_str(), board.c_str());
		}

		void EventNetStatus(enum StatusNetwork netstate) override
		{
			owner.publish(Events::HubNetStatus{handle, converted(netstate), isSecure()});

			/* What a hub agreed to does not outlive the connection it was agreed on. */
			if (owner.boards && netstate != NetLookup && netstate != NetConnecting
			    && netstate != NetConnected && netstate != NetTlsConnected)
				owner.boards->available(handle, false);
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
			if (owner.boards) owner.boards->available(handle, available);
		}

		void EventBoardDescribed(const QuickDC::Board& board) override
		{
			if (owner.boards) owner.boards->described(handle, board);
		}

		void EventBoardEntry(const QuickDC::BoardEntry& entry) override
		{
			if (owner.boards) owner.boards->arrived(handle, entry);
		}

		void EventBoardRefused(int code, const char* message, const char* board,
		                       const char* tth) override
		{
			owner.publish(Events::BoardRefused{handle, code, text(message), text(board), text(tth)});
			if (owner.boards) owner.boards->refused(handle, code, text(board), text(tth));
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
			const std::string sender = line.from.sourceId;
			owner.publish(Events::ChatReceived{handle, std::move(line)});
			if (richText) owner.chatMediaIn(handle, text(getUrl()), sender, text(message));
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

			const std::string sender = line.from.sourceId;
			owner.publish(Events::ChatReceived{handle, std::move(line)});
			if (richText) owner.chatMediaIn(handle, text(getUrl()), sender, text(message));
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
			if (richText) owner.chatMediaIn(handle, text(getUrl()), std::string(), text(message));
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

			const uint64_t search = owner.searches.attribute(*result);
			if (!search) return;

			SearchHit hit;
			hit.hubUrl = text(getUrl());
			hit.name = text(result->getFile());
			hit.tth = text(result->getTTH());
			hit.size = result->getSize();
			hit.freeSlots = static_cast<uint32_t>(result->getFreeSlots());
			hit.user = converted(result->getUser());
			owner.publish(Events::SearchResult{handle, search, std::move(hit)});
		}

		void EventHubName(const char* hubname) override
		{
			owner.publish(Events::HubName{handle, text(hubname)});
			owner.hubNamed(opened, text(hubname));
		}

		void EventHubDescription(const char* description) override
		{
			owner.publish(Events::HubDescription{handle, text(description)});
		}

		void EventHubRedirect(const char* address, uint16_t port, const char* following,
		                      QuickDC::RedirectRefusal why) override
		{
			const std::string to = text(following);
			if (!to.empty())
			{
				if (owner.boards)
				{
					owner.boards->closed(handle);
					owner.boards->opened(handle, this, to);
				}
				if (owner.recents)
				{
					owner.recents->visited(to, static_cast<uint64_t>(time(0)));
					owner.publish(owner.recentHubs(0));
				}
			}
			owner.publish(Events::HubRedirect{handle, text(address), port, !to.empty(), to, converted(why)});
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
		/** The address it was opened by, which is what the visit was recorded under. */
		std::string opened;
		bool allowPeerConnections = true;
};

QuickDC::Api::Service::Service(Core& core_)
	: core(core_)
	, dropObserved(observeDropCheck(core_, this))
	, queueObserved(core_.queue ? core_.queue->subscribe(this) : Subscription())
	, transfersObserved(core_.transfers ? core_.transfers->subscribe(this) : Subscription())
	, configObserved(core_.config ? core_.config->subscribe(this) : Subscription())
	, sessionObserved(core_.session ? core_.session->subscribe(this) : Subscription())
{
	if (core.shares) core.shares->setHashedCallback([this] { publish(shareStatus(0)); });

	/* A write the core made of its own accord is answered as request 0. */
	if (core.settingsKeeper)
		core.settingsKeeper->written = [this](const Preferences& store, bool ok) {
			publish(Events::SettingsSaved{&store == core.session ? SettingsStore::Session
			                                                     : SettingsStore::Application, 0, ok});
		};

	if (core.config)
	{
		favourites = std::make_unique<QuickDC::FavouriteHubs>(*core.config);
		recents = std::make_unique<QuickDC::RecentHubs>(*core.config);
		completed = std::make_unique<QuickDC::CompletedDownloads>(*core.config);
		directory = std::make_unique<QuickDC::HubDirectory>(core.loop(), *core.config,
			core.homeDirectory.empty() ? std::string() : core.homeDirectory + "/hublists",
			[this] { publish(hubListSources(0)); }, [this] { publish(hubDirectory(0)); });
		boards = std::make_unique<QuickDC::BoardKeeper>(core, core.homeDirectory,
			[this](const std::string& url) {
				const std::string group = hubSettingsGroup(url);
				for (const std::string& kept : favourites->urls())
					if (hubSettingsGroup(kept) == group) return true;
				return false;
			},
			[this](const Event& event) { publish(event); });
		sweepPasted(mediaFolder(), Settings::read(*core.config, Settings::ChatMediaKeepDays), time(0));
	}
}

QuickDC::Api::Service::~Service()
{
	if (core.settingsKeeper) core.settingsKeeper->written = nullptr;

	/* An attachment still being hashed would otherwise report to this. */
	if (core.shares)
	{
		core.shares->forgetListener(this);
		core.shares->setHashedCallback(nullptr);
	}
}

void QuickDC::Api::Service::execute(const Command& command)
{
	std::visit([this](const auto& c) { run(c); }, command.value);
	announceState();
}

void QuickDC::Api::Service::announceState()
{
	if (connectionModeChanged)
	{
		connectionModeChanged = false;
		publish(connectionMode(0));
	}
}

std::vector<QuickDC::Api::Event> QuickDC::Api::Service::state(uint64_t request) const
{
	return {coreStatus(request), connectionMode(request), shareFolders(request), favouriteHubs(request),
	        recentHubs(request), completedDownloads(request), hubListSources(request),
	        hubDirectory(request)};
}

void QuickDC::Api::Service::run(const Commands::ReportState& command)
{
	for (const Event& event : state(command.request)) publish(event);
}

QuickDC::Api::Events::ConnectionMode QuickDC::Api::Service::connectionMode(uint64_t request) const
{
	Events::ConnectionMode told;
	told.request = request;
	if (!core.config) return told;

	const NetworkModeKeys keys = networkModeKeysInForce(*core.config);
	told.mode = networkModeInForce(*core.config);
	told.activeMode = keys.activeMode;
	told.useUpnp = keys.useUpnp;
	told.forceActive = keys.forceActive;
	told.reachabilityServer = keys.reachabilityServer;
	return told;
}

void QuickDC::Api::Service::run(const Commands::SetConnectionMode& command)
{
	if (core.config)
	{
		writing = command.origin;
		setNetworkMode(*core.config, command.mode, command.useUpnp, command.forceActive);
		writing = 0;
	}

	/* Told even where nothing changed, so the front end that asked is answered. */
	connectionModeChanged = true;
	publish(Events::SettingsWritten{SettingsStore::Application, command.origin});
}

void QuickDC::Api::Service::publish(const Event& event)
{
	sinks.notify([&event](EventSink& sink) { sink.deliver(event); });
}

void QuickDC::Api::Service::run(const Commands::FetchHubList& command)
{
	std::unique_ptr<HubListFetch>& fetch = hublists[command.url];
	if (!fetch) fetch = std::make_unique<HubListFetch>(*this, command.url);

	/* Asked again while on its way: the answer coming is the answer for this too. */
	if (fetch->busy()) return;

	fetch->start();
}

QuickDC::Api::Events::HubListSources QuickDC::Api::Service::hubListSources(uint64_t request) const
{
	Events::HubListSources told;
	told.request = request;
	if (directory) told.sources = directory->sources();
	return told;
}

QuickDC::Api::Events::HubDirectory QuickDC::Api::Service::hubDirectory(uint64_t request) const
{
	Events::HubDirectory told;
	told.request = request;
	if (directory)
	{
		told.sources = directory->mergedFrom();
		told.hubs = directory->hubs();
	}
	return told;
}

void QuickDC::Api::Service::run(const Commands::AddHubListSource& command)
{
	if (directory) directory->add(command.url, static_cast<uint64_t>(time(0)));
}

void QuickDC::Api::Service::run(const Commands::RemoveHubListSource& command)
{
	if (directory) directory->remove(command.url);
}

void QuickDC::Api::Service::run(const Commands::ResetHubListSources&)
{
	if (directory) directory->reset();
}

void QuickDC::Api::Service::run(const Commands::RefreshHubDirectory& command)
{
	if (directory) directory->refresh(command.force, static_cast<uint64_t>(time(0)));
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

void QuickDC::Api::Service::run(const Commands::AttachPasted& command)
{
	const std::string folder = mediaFolder();
	if (folder.empty() || command.data.empty())
	{
		publish(Events::PastedKept{command.request, std::string(), "There is nowhere to keep a pasted picture."});
		return;
	}

	std::error_code error;
	std::filesystem::create_directories(folder, error);
	const std::string path = folder + "/" + pastedName(time(0), command.suffix);

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(command.data.data(), static_cast<std::streamsize>(command.data.size()));
	out.close();
	if (error || !out)
	{
		publish(Events::PastedKept{command.request, std::string(),
		                           "A pasted picture could not be written to '" + path + "'."});
		return;
	}

	publish(Events::PastedKept{command.request, path, std::string()});
	run(Commands::AttachFile{path, true, false});
}

std::string QuickDC::Api::Service::mediaFolder() const
{
	return core.homeDirectory.empty() ? std::string() : core.homeDirectory + "/media";
}

std::string QuickDC::Api::Service::downloadDirectory() const
{
	std::string directory = core.config
		? core.config->peekString(Settings::DownloadDirectory.group, Settings::DownloadDirectory.key, "")
		: std::string();

	if (directory.empty())
		directory = core.homeDirectory.empty() ? std::string(Settings::DownloadDirectory.fallback)
		                                       : core.homeDirectory + "/downloads";

	return Samurai::IO::File::resolvePath(directory);
}

std::string QuickDC::Api::Service::localCopy(const std::string& tth) const
{
	std::error_code error;
	const auto here = [&error](const std::string& path) {
		return !path.empty() && std::filesystem::is_regular_file(Samurai::IO::File::resolvePath(path), error);
	};

	if (core.shares)
		if (const std::string attached = core.shares->attachedPath(tth); here(attached))
			return Samurai::IO::File::resolvePath(attached);

	if (completed)
		for (const QuickDC::CompletedDownload& done : completed->entries())
			if (done.tth == tth && here(done.filename))
				return Samurai::IO::File::resolvePath(done.filename);

	return std::string();
}

void QuickDC::Api::Service::chatMediaIn(HubHandle hub, const std::string& hubUrl, const std::string& sender,
                                        const std::string& text)
{
	if (!core.config || !core.session) return;

	const bool fetching = chatMediaFetchedOn(*core.config, *core.session, hubUrl);
	const int limit = Settings::read(*core.config, Settings::ChatMediaLimit);

	for (const ChatAttachment& file : attachmentsIn(text))
	{
		if (!file.inlineMedia) continue;

		using Media = Events::ChatMedia;
		if (const std::string path = localCopy(file.tth); !path.empty())
		{
			publish(Media{hub, file.tth, file.name, file.size, Media::State::Ready, Media::Why::None, path});
			continue;
		}

		switch (chatMediaVerdict(file.size, fetching, limit > 0 ? static_cast<uint64_t>(limit) : 0))
		{
			case ChatMediaSkip::None:
				fetchMedia(hub, file, hubUrl, sender);
				break;
			case ChatMediaSkip::Off:
				publish(Media{hub, file.tth, file.name, file.size, Media::State::Skipped, Media::Why::Off, {}});
				break;
			case ChatMediaSkip::Unsized:
				publish(Media{hub, file.tth, file.name, file.size, Media::State::Skipped, Media::Why::Unsized, {}});
				break;
			case ChatMediaSkip::TooLarge:
				publish(Media{hub, file.tth, file.name, file.size, Media::State::Skipped, Media::Why::TooLarge, {}});
				break;
		}
	}
}

void QuickDC::Api::Service::fetchMedia(HubHandle hub, const ChatAttachment& file, const std::string& hubUrl,
                                       const std::string& sender)
{
	if (!core.queue) return;

	using Media = Events::ChatMedia;
	Media told{hub, file.tth, file.name, file.size, Media::State::Queued, Media::Why::None, {}};

	if (QueueFile* queued = core.queue->getFileByTTH(file.tth.c_str()))
	{
		told.path = queued->getLocalFilename() ? queued->getLocalFilename() : "";
	}
	else
	{
		told.path = downloadDirectory() + "/" + file.name;
		if (!core.queue->add(told.path.c_str(), file.tth.c_str(), file.size))
		{
			publish(Media{hub, file.tth, file.name, file.size, Media::State::Skipped, Media::Why::Refused, {}});
			return;
		}
		if (!hubUrl.empty() && !sender.empty())
			core.queue->addKnownSource(file.tth.c_str(), hubUrl.c_str(), sender.c_str());
		core.queue->requestSourceSearch(file.tth.c_str());
	}

	mediaFetches[file.tth] = MediaFetch{hub, file.name};
	publish(told);
}

void QuickDC::Api::Service::run(const Commands::FetchAttachment& command)
{
	ChatAttachment file;
	file.tth = command.tth;
	file.size = command.size;
	file.name = readMagnet("magnet:?dn=" + command.name).name;
	if (file.tth.size() != 39) return;
	if (file.name.empty()) file.name = file.tth;

	if (const std::string path = localCopy(file.tth); !path.empty())
	{
		using Media = Events::ChatMedia;
		publish(Media{command.hub, file.tth, file.name, file.size, Media::State::Ready, Media::Why::None, path});
		return;
	}

	HubSession* hub = command.hub ? session(command.hub) : nullptr;
	fetchMedia(command.hub, file, hub ? text(hub->getUrl()) : std::string(), std::string());
}

void QuickDC::Api::Service::run(const Commands::SetChatMediaFetch& command)
{
	if (!core.session) return;

	const std::string group = hubSettingsGroup(command.hubUrl);
	if (group.empty()) return;

	using Choice = Commands::SetChatMediaFetch::Choice;
	if (command.choice == Choice::Follow)
		core.session->remove(group, Settings::ChatMediaFetch.key);
	else
		core.session->setBool(group, Settings::ChatMediaFetch.key, command.choice == Choice::Always);
	core.session->write();
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

	Request request(policy, command.size, nullptr, nullptr, nullptr);
	for (const std::string& term : command.include) request.addInclusion(term.c_str());
	for (const std::string& term : command.exclude) request.addExclusion(term.c_str());
	for (const std::string& ext : command.extensions) request.addExtension(ext.c_str());

	const int asked = static_cast<int>(core.hubs->searchAllHubs(&request));
	publish(Events::SearchSent{command.request, searches.add(request), asked});
}

void QuickDC::Api::Service::run(const Commands::ForgetSearch& command)
{
	searches.remove(command.search);
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

	const std::string id = core.queue->sourceIdOn(command.hubUrl.c_str(), command.sourceId.c_str());
	if (file->findSource(command.hubUrl.c_str(), id.c_str())) return;

	file->addSource(new FileSource(command.hubUrl.c_str(), id.c_str(),
		command.remoteName.empty() ? nullptr : command.remoteName.c_str()));
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
	if (file->getTTH()) mediaFetches.erase(file->getTTH());

	core.queue->remove(file);
	if (!partial.empty()) Samurai::IO::File(partial).remove();
}

void QuickDC::Api::Service::run(const Commands::SnapshotTransfers& command)
{
	Events::TransfersSnapshot taken;
	taken.request = command.request;

	/* A post somebody is reading is not a download anybody chose, so it is not listed as one. */
	const auto listed = [this](const std::string& filename) { return !boards || !boards->isFetch(filename); };

	if (core.transfers)
		for (QuickDC::Transfer* t = core.transfers->first(); t; t = core.transfers->next())
		{
			Transfer row = converted(t);
			if (t->isUpload() || listed(row.filename)) taken.active.push_back(std::move(row));
		}

	if (core.queue)
	{
		std::vector<QueueStatus> status;
		core.queue->getStatus(status);
		taken.queued.reserve(status.size());
		size_t reading = 0;
		for (const QueueStatus& entry : status)
		{
			if (listed(entry.filename ? entry.filename : "")) taken.queued.push_back(converted(entry));
			else reading += entry.activeSources;
		}
		const size_t active = core.queue->countActiveTransfers();
		taken.activeDownloads = static_cast<uint32_t>(active > reading ? active - reading : 0);
	}

	publish(taken);
}

QuickDC::Api::Events::ShareStatus QuickDC::Api::Service::shareStatus(uint64_t request) const
{
	Events::ShareStatus status;
	status.request = request;

	if (core.shares)
	{
		status.files = core.shares->getFiles();
		status.size = core.shares->getSharedSize();
	}

	status.hashing = core.hash && core.hash->isBusy();
	return status;
}

void QuickDC::Api::Service::run(const Commands::ReportShare& command)
{
	publish(shareStatus(command.request));
}

QuickDC::Api::Events::CoreStatus QuickDC::Api::Service::coreStatus(uint64_t request) const
{
	Events::CoreStatus status = listening();
	status.request = request;
	status.version = core.getVersion();
	status.started = static_cast<uint64_t>(core.startup.getInternalData());
	return status;
}

QuickDC::Api::Events::CoreStatus QuickDC::Api::Service::listening() const
{
	Events::CoreStatus status;
	if (core.server)
	{
		status.port = core.server->boundPort();
		status.tcp = core.server->listening(false);
		status.tcp6 = core.server->listening(true);
		status.udp = core.server->listeningUdp(false);
		status.udp6 = core.server->listeningUdp(true);
	}

	status.localHub = core.localHub && core.localHub->enabled && (status.tcp || status.tcp6);
	return status;
}

void QuickDC::Api::Service::checkStatus()
{
	const Events::CoreStatus now = listening();
	const auto same = [](const Events::CoreStatus& a, const Events::CoreStatus& b) {
		return a.port == b.port && a.tcp == b.tcp && a.tcp6 == b.tcp6 && a.udp == b.udp
			&& a.udp6 == b.udp6 && a.localHub == b.localHub;
	};

	if (statusTold && same(*statusTold, now)) return;
	statusTold = now;
	publish(coreStatus(0));
}

void QuickDC::Api::Service::run(const Commands::ReportStatus& command)
{
	publish(coreStatus(command.request));
}

void QuickDC::Api::Service::run(const Commands::SnapshotConnections& command)
{
	Events::ConnectionsSnapshot told;
	told.request = command.request;

	if (core.connections)
	{
		for (QuickDC::Connection* connection = core.connections->first(); connection;
		     connection = core.connections->next())
		{
			PeerConnection out;
			out.protocol = text(connection->getProtocol());
			out.peer = connection->peerName();

			if (const Samurai::IO::Net::Socket* socket = connection->getSocket())
			{
				const Samurai::IO::Net::InetAddress* address = socket->getAddress();
				if (address) out.address = address->toString();
				out.port = socket->getPort();
				out.secure = socket->isSecure();
				out.outbound = socket->isOutbound();
			}

			told.connections.push_back(std::move(out));
		}
	}

	publish(std::move(told));
}

void QuickDC::Api::Service::run(const Commands::FindShared& command)
{
	Events::SharedFile told;
	told.request = command.request;
	told.tth = command.tth;

	if (core.shares)
	{
		if (const std::optional<Share::Manager::Offer> offer = core.shares->offered(command.tth))
		{
			told.found = true;
			told.path = offer->path;
			told.name = offer->name;
			told.size = offer->size;
			told.attached = offer->attached;
		}
	}

	publish(std::move(told));
}

void QuickDC::Api::Service::run(const Commands::KickUser& command)
{
	using Why = Events::KickRefused::Why;

	HubSession* hub = session(command.hub);
	const auto refused = [&](Why why) { publish(Events::KickRefused{command.hub, command.userId, why}); };
	if (!hub) return refused(Why::NoSession);

	switch (hub->disconnectUser(command.userId.c_str(), command.message, command.redirect, command.banSeconds))
	{
		case QuickDC::Hub::DisconnectRefusal::None:        return;
		case QuickDC::Hub::DisconnectRefusal::NoSession:   return refused(Why::NoSession);
		case QuickDC::Hub::DisconnectRefusal::NoSuchUser:  return refused(Why::NoSuchUser);
		case QuickDC::Hub::DisconnectRefusal::NotOperator: return refused(Why::NotOperator);
		case QuickDC::Hub::DisconnectRefusal::Unsupported: return refused(Why::Unsupported);
	}
}

namespace {

QuickDC::Api::LocalHubCredential converted(ADC::LocalHub::Credentials credentials)
{
	switch (credentials)
	{
		case ADC::LocalHub::Cred_User:     return QuickDC::Api::LocalHubCredential::User;
		case ADC::LocalHub::Cred_Operator: return QuickDC::Api::LocalHubCredential::Operator;
		case ADC::LocalHub::Cred_Admin:    return QuickDC::Api::LocalHubCredential::Admin;
	}
	return QuickDC::Api::LocalHubCredential::User;
}

QuickDC::Api::LocalHubUser::State converted(ADC::LocalHub::State state)
{
	using Out = QuickDC::Api::LocalHubUser::State;
	switch (state)
	{
		case ADC::LocalHub::State_None:
		case ADC::LocalHub::State_Protocol:
		case ADC::LocalHub::State_Identify:       return Out::LoggingIn;
		case ADC::LocalHub::State_Verify:         return Out::Verifying;
		case ADC::LocalHub::State_Normal:         return Out::Normal;
		case ADC::LocalHub::State_WaitDisconnect: return Out::Leaving;
	}
	return Out::LoggingIn;
}

/* The first identity of 'type' the entry has, or empty. */
std::string identity(const ADC::LocalHub::ACL::Authorization& entry, ADC::LocalHub::ACL::Identity::IdentityType type)
{
	for (const ADC::LocalHub::ACL::Identity* id : entry.getIdentities())
		if (id->getType() == type) return id->getData();
	return std::string();
}

}

QuickDC::Api::Events::LocalHubState QuickDC::Api::Service::localHubState(uint64_t request) const
{
	using ADC::LocalHub::ACL::Authorization;
	using ADC::LocalHub::ACL::Identity;

	Events::LocalHubState told;
	told.request = request;

	ADC::LocalHub::Hub* hub = core.localHub;
	if (!hub) return told;

	told.enabled = hub->enabled;
	told.name = text(hub->hubname);
	told.maxUsers = hub->maxUsers;
	told.started = static_cast<uint64_t>(hub->startedAt().getInternalData());

	for (ADC::LocalHub::User* user : hub->users)
	{
		LocalHubUser out;
		out.sid = ADC::SID::toString(user->getSID());
		out.nick = text(user->getNick());
		out.cid = text(user->getCID());
		out.address = user->getObservedAddress();
		out.credential = converted(user->getCredentials());
		out.state = converted(user->getState());
		out.sendQueue = user->getSendQueueSize();
		told.users.push_back(std::move(out));
	}

	const ADC::LocalHub::ACL::Table& acl = hub->accessList();
	for (size_t n = 0; n < acl.countUsers(); ++n)
	{
		const Authorization* entry = acl.getUser(n);
		if (!entry || entry->isExpired()) continue;

		if (entry->getCredential() == Authorization::Banned)
		{
			LocalHubBan ban;
			ban.name = text(entry->getName());
			ban.reason = text(entry->getDescription());
			ban.until = static_cast<uint64_t>(entry->getExpireTime().getInternalData());
			ban.cid = identity(*entry, Identity::IdCID);
			ban.nick = identity(*entry, Identity::IdName);
			ban.address = identity(*entry, Identity::IdIPv4);
			if (ban.address.empty()) ban.address = identity(*entry, Identity::IdIPv6);
			told.bans.push_back(std::move(ban));
			continue;
		}

		if (!entry->password()) continue;

		LocalHubAccount account;
		account.nick = identity(*entry, Identity::IdName);
		if (account.nick.empty()) account.nick = text(entry->getName());
		switch (entry->getCredential())
		{
			case Authorization::Admin:    account.credential = LocalHubCredential::Admin; break;
			case Authorization::Operator: account.credential = LocalHubCredential::Operator; break;
			case Authorization::User:     account.credential = LocalHubCredential::User; break;
			default: continue;
		}
		told.accounts.push_back(std::move(account));
	}

	return told;
}

void QuickDC::Api::Service::localHubAnswered(bool done, Events::LocalHubRefused::Why why, uint64_t request)
{
	if (done) publish(localHubState(request));
	else publish(Events::LocalHubRefused{request, why});
}

void QuickDC::Api::Service::run(const Commands::ReportLocalHub& command)
{
	localHubAnswered(core.localHub != nullptr, Events::LocalHubRefused::Why::NoHub, command.request);
}

void QuickDC::Api::Service::run(const Commands::KickFromLocalHub& command)
{
	using Why = Events::LocalHubRefused::Why;
	if (!core.localHub) return localHubAnswered(false, Why::NoHub, command.request);

	ADC::LocalHub::User* user = core.localHub->findUser(command.who);
	if (user) core.localHub->disconnect(user, command.message.empty() ? "Kicked" : command.message, std::string(), 0);
	localHubAnswered(user != nullptr, Why::NoSuchUser, command.request);
}

void QuickDC::Api::Service::run(const Commands::BanFromLocalHub& command)
{
	using Why = Events::LocalHubRefused::Why;
	if (!core.localHub) return localHubAnswered(false, Why::NoHub, command.request);
	localHubAnswered(core.localHub->ban(command.who, command.reason, command.seconds), Why::Invalid,
	                 command.request);
}

void QuickDC::Api::Service::run(const Commands::UnbanFromLocalHub& command)
{
	using Why = Events::LocalHubRefused::Why;
	if (!core.localHub) return localHubAnswered(false, Why::NoHub, command.request);
	localHubAnswered(core.localHub->unban(command.who), Why::NoSuchEntry, command.request);
}

void QuickDC::Api::Service::run(const Commands::SetLocalHubAccount& command)
{
	using Why = Events::LocalHubRefused::Why;
	using ADC::LocalHub::ACL::Authorization;
	if (!core.localHub) return localHubAnswered(false, Why::NoHub, command.request);
	if (command.nick.empty() || command.password.empty()) return localHubAnswered(false, Why::Invalid, command.request);

	Authorization::Credential rights = Authorization::User;
	if (command.credential == LocalHubCredential::Operator) rights = Authorization::Operator;
	else if (command.credential == LocalHubCredential::Admin) rights = Authorization::Admin;

	localHubAnswered(core.localHub->setAccount(command.nick, command.password, rights), Why::NotSaved,
	                 command.request);
}

void QuickDC::Api::Service::run(const Commands::RemoveLocalHubAccount& command)
{
	using Why = Events::LocalHubRefused::Why;
	if (!core.localHub) return localHubAnswered(false, Why::NoHub, command.request);
	localHubAnswered(core.localHub->removeAccount(command.nick), Why::NoSuchEntry, command.request);
}

QuickDC::Api::Events::ShareFolders QuickDC::Api::Service::shareFolders(uint64_t request) const
{
	Events::ShareFolders told;
	told.request = request;

	if (core.shares)
		for (const Share::Folder& folder : core.shares->getFolders())
			told.folders.push_back(ShareFolder{folder.name, folder.path});

	return told;
}

void QuickDC::Api::Service::foldersAnswered(Share::FolderRefusal refusal, const std::string& path,
                                            uint64_t request)
{
	using Refused = Events::ShareFolderRefused;
	using Why = Share::FolderRefusal;

	switch (refusal)
	{
		case Why::None:
			publish(shareFolders(request));
			publish(shareStatus(request));
			if (core.hubs) core.hubs->requestClientInfoUpdate();
			return;

		case Why::AlreadyShared: publish(Refused{request, path, Refused::Why::AlreadyShared}); return;
		case Why::InsideShared:  publish(Refused{request, path, Refused::Why::InsideShared});  return;
		case Why::NotADirectory: publish(Refused{request, path, Refused::Why::NotADirectory}); return;
		case Why::NotShared:     publish(Refused{request, path, Refused::Why::NotShared});     return;
		case Why::NoName:        publish(Refused{request, path, Refused::Why::NoName});        return;
	}
}

void QuickDC::Api::Service::run(const Commands::AddShareFolder& command)
{
	if (!core.shares) return;
	const Share::FolderRefusal refusal = core.shares->addFolder(command.path, command.name, core.transfers);
	foldersAnswered(refusal, command.path, command.request);
}

void QuickDC::Api::Service::run(const Commands::RemoveShareFolder& command)
{
	if (!core.shares) return;
	const Share::FolderRefusal refusal = core.shares->removeFolder(command.path, core.transfers);
	foldersAnswered(refusal, command.path, command.request);
}

void QuickDC::Api::Service::run(const Commands::RenameShareFolder& command)
{
	if (!core.shares) return;
	const Share::FolderRefusal refusal =
		core.shares->renameFolder(command.path, command.name, core.transfers);
	foldersAnswered(refusal, command.path, command.request);
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

	/* A post's document is the store's, and not a download anybody chose. */
	if (boards && file->getLocalFilename()
	    && boards->downloaded(file->getLocalFilename(), file->getTTH() ? file->getTTH() : ""))
		return;

	const Events::DownloadCompleted done{
		file->getLocalFilename() ? file->getLocalFilename() : "",
		file->getSize(),
		file->getTTH() ? file->getTTH() : ""};
	publish(done);

	if (completed && completed->add(QuickDC::CompletedDownload{
	        done.filename, done.tth, done.size, static_cast<uint64_t>(time(0))}))
		publish(completedDownloads(0));

	const auto fetched = mediaFetches.find(done.tth);
	if (fetched == mediaFetches.end()) return;

	const std::string path = Samurai::IO::File::resolvePath(done.filename);
	publish(Events::ChatMedia{fetched->second.hub, done.tth, fetched->second.name, done.size,
	                          Events::ChatMedia::State::Ready, Events::ChatMedia::Why::None, path});
	mediaFetches.erase(fetched);

	/* Whoever else was shown it may ask this client for it now. */
	run(Commands::AttachFile{path, true, false});
}

void QuickDC::Api::Service::EventTransferStarted(QuickDC::Transfer* transfer)
{
	publish(Events::TransferStarted{converted(transfer)});
}

void QuickDC::Api::Service::EventTransferFinished(QuickDC::Transfer* transfer)
{
	publish(Events::TransferFinished{converted(transfer)});
	if (boards && transfer && transfer->isUpload()) boards->transfersChanged();
}

void QuickDC::Api::Service::EventTransferStopped(QuickDC::Transfer* transfer)
{
	publish(Events::TransferStopped{converted(transfer)});
	if (boards && transfer && transfer->isUpload()) boards->transfersChanged();
}

void QuickDC::Api::Service::EventTransferError(QuickDC::Transfer* transfer)
{
	publish(Events::TransferError{converted(transfer)});
	if (boards && transfer && transfer->isUpload()) boards->transfersChanged();
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

	auto made = std::make_unique<HubSession>(*this, command.hub, &parsed, command.url);
	if (boards) boards->opened(command.hub, made.get(), command.url);
	hubs[command.hub] = std::move(made);

	if (recents)
	{
		recents->visited(command.url, static_cast<uint64_t>(time(0)));
		publish(recentHubs(0));
	}
}

void QuickDC::Api::Service::hubNamed(const std::string& url, const std::string& name)
{
	if (recents && !name.empty() && recents->named(url, name)) publish(recentHubs(0));
}

QuickDC::Api::Events::FavouriteHubs QuickDC::Api::Service::favouriteHubs(uint64_t request) const
{
	Events::FavouriteHubs told;
	told.request = request;
	if (favourites) told.urls = favourites->urls();
	return told;
}

QuickDC::Api::Events::RecentHubs QuickDC::Api::Service::recentHubs(uint64_t request) const
{
	Events::RecentHubs told;
	told.request = request;
	if (recents)
		for (const QuickDC::RecentHub& hub : recents->entries())
			told.hubs.push_back(Api::RecentHub{hub.url, hub.name, hub.visited});
	return told;
}

QuickDC::Api::Events::CompletedDownloads QuickDC::Api::Service::completedDownloads(uint64_t request) const
{
	Events::CompletedDownloads told;
	told.request = request;
	if (completed)
		for (const QuickDC::CompletedDownload& one : completed->entries())
			told.downloads.push_back(Api::CompletedDownload{one.filename, one.tth, one.size, one.finished});
	return told;
}

void QuickDC::Api::Service::run(const Commands::AddFavouriteHub& command)
{
	if (favourites && favourites->add(command.url))
	{
		publish(favouriteHubs(0));
		if (boards) boards->policyChanged();
	}
}

void QuickDC::Api::Service::run(const Commands::RemoveFavouriteHub& command)
{
	if (favourites && favourites->remove(command.url))
	{
		publish(favouriteHubs(0));
		if (boards) boards->policyChanged();
	}
}

void QuickDC::Api::Service::run(const Commands::ForgetRecentHub& command)
{
	if (recents && recents->remove(command.url)) publish(recentHubs(0));
}

void QuickDC::Api::Service::run(const Commands::ClearRecentHubs&)
{
	if (recents && recents->clear()) publish(recentHubs(0));
}

void QuickDC::Api::Service::run(const Commands::ForgetCompletedDownload& command)
{
	if (completed && completed->remove(command.filename)) publish(completedDownloads(0));
}

void QuickDC::Api::Service::run(const Commands::ClearCompletedDownloads&)
{
	if (completed && completed->clear()) publish(completedDownloads(0));
}

/* Disconnected first: ~Hub frees the session, and does not close it. */
void QuickDC::Api::Service::run(const Commands::CloseHub& command)
{
	const auto found = hubs.find(command.hub);
	if (found == hubs.end()) return;

	std::unique_ptr<HubSession> closing = std::move(found->second);
	hubs.erase(found);
	if (boards) boards->closed(command.hub);
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
	if (boards) boards->subscribe(command.hubUrl, command.board, command.fromStart);
}

void QuickDC::Api::Service::run(const Commands::UnsubscribeBoard& command)
{
	if (boards) boards->unsubscribe(command.hubUrl, command.board);
}

void QuickDC::Api::Service::run(const Commands::RequestBoardEntry& command)
{
	if (boards) boards->requestEntry(command.hubUrl, command.board, command.tth);
}

void QuickDC::Api::Service::run(const Commands::WritePost& command)
{
	if (boards) boards->write(command);
	else publish(Events::PostRefused{command.request, Events::PostRefused::Why::NoHub, 0, 0, std::string()});
}

void QuickDC::Api::Service::run(const Commands::WithdrawPost& command)
{
	if (boards) boards->withdraw(command.hubUrl, command.board, command.tth);
}

void QuickDC::Api::Service::run(const Commands::LoadBoard& command)
{
	if (boards) boards->load(command.hubUrl, command.board);
}

void QuickDC::Api::Service::run(const Commands::ReadPost& command)
{
	if (boards) boards->read(command.hubUrl, command.board, command.tth, command.fetch);
}

void QuickDC::Api::Service::run(const Commands::MarkRead& command)
{
	if (boards) boards->markRead(command.hubUrl, command.board, command.tth);
}

/* Nothing written is what following the general setting means; the change is heard as a setting. */
void QuickDC::Api::Service::run(const Commands::SetBoardPolicy& command)
{
	if (!core.session) return;

	const std::string group = hubSettingsGroup(command.hubUrl);
	if (group.empty()) return;

	using Policy = Commands::SetBoardPolicy::Policy;
	using Choice = Commands::SetBoardPolicy::Choice;
	const char* key = command.policy == Policy::Serve ? Settings::BoardServe.key : Settings::BoardPrefetch.key;

	if (command.choice == Choice::Follow) core.session->remove(group, key);
	else core.session->setBool(group, key, command.choice == Choice::Always);
	core.session->write();
}

QuickDC::Preferences* QuickDC::Api::Service::preferences(SettingsStore store) const
{
	return store == SettingsStore::Session ? core.session : core.config;
}

QuickDC::Api::SettingsSnapshot QuickDC::Api::Service::settingsSnapshot() const
{
	SettingsSnapshot taken;

	const auto copy = [](const Preferences* from, SettingsGroups& into) {
		if (!from) return;
		for (const auto& [group, settings] : from->snapshot())
			for (const auto& [key, value] : settings) into[group][key] = value;
	};

	copy(core.config, taken.application);
	copy(core.session, taken.session);
	taken.applicationRevision = core.config ? core.config->revision() : 0;
	taken.sessionRevision = core.session ? core.session->revision() : 0;
	return taken;
}

void QuickDC::Api::Service::EventSettingChanged(const Preferences& store, const std::string& group,
                                                const std::string& key, const std::string* value)
{
	Events::SettingChanged changed;
	changed.store = &store == core.session ? SettingsStore::Session : SettingsStore::Application;
	changed.group = group;
	changed.key = key;
	changed.removed = value == nullptr;
	if (value) changed.value = *value;
	changed.revision = store.revision();
	changed.origin = writing;
	publish(changed);

	namespace S = QuickDC::Settings;
	if (boards && ((&store == core.config && group == S::BoardServe.group)
	               || (&store == core.session && (key == S::BoardServe.key || key == S::BoardPrefetch.key))))
		boards->policyChanged();

	if (&store == core.config && group == S::ConnectionMode.group
	    && (key == S::ConnectionMode.key || key == S::ActiveMode.key || key == S::UseUpnp.key
	        || key == S::ForceActiveMode.key || key == S::ReachabilityServer.key))
		connectionModeChanged = true;
}

void QuickDC::Api::Service::run(const Commands::SetSetting& command)
{
	if (Preferences* store = preferences(command.store))
	{
		writing = command.origin;
		if (command.remove) store->remove(command.group, command.key);
		else store->setString(command.group, command.key, command.value);
		writing = 0;
	}

	publish(Events::SettingsWritten{command.store, command.origin});
}

void QuickDC::Api::Service::run(const Commands::ReplaceGroup& command)
{
	if (Preferences* store = preferences(command.store))
	{
		writing = command.origin;
		for (const std::string& key : command.removeKeys) store->remove(command.group, key);
		for (const auto& [key, value] : command.values) store->setString(command.group, key, value);
		writing = 0;
	}

	publish(Events::SettingsWritten{command.store, command.origin});
}

void QuickDC::Api::Service::run(const Commands::SaveSettings& command)
{
	Preferences* store = preferences(command.store);
	publish(Events::SettingsSaved{command.store, command.request, store && store->write()});
}

