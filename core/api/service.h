/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_SERVICE_H
#define HAVE_QUICKDC_API_SERVICE_H

#include "api/attachments.h"
#include "chat/chatmedia.h"
#include "api/protocol.h"
#include "config/preferences.h"
#include "network/downloadqueue.h"
#include "network/transfermanager.h"
#include "share/searchmanager.h"
#include "share/sharefolders.h"
#include "config/userlists.h"
#include "network/hubdirectory.h"
#include "api/subscription.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

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
class Service : private DropCheckListener, private Share::AttachmentListener,
                private QueueListener, private TransferListener,
                private PreferencesListener {
	public:
		explicit Service(Core& core);
		~Service() override;

		Service(const Service&) = delete;
		Service& operator=(const Service&) = delete;

		void execute(const Command& command);

		Subscription subscribe(EventSink* sink) { return sinks.subscribe(sink); }

		void publish(const Event& event);

		/** Both preference stores as they stand. Core thread only. */
		SettingsSnapshot settingsSnapshot() const;

		/** What ReportState tells, with 'request' in each. Core thread only. */
		std::vector<Event> state(uint64_t request) const;

	private:
		class HubListFetch;
		class HubSession;
		class BoardStore;

		void run(const Commands::FetchHubList& command);
		void run(const Commands::AddHubListSource& command);
		void run(const Commands::RemoveHubListSource& command);
		void run(const Commands::ResetHubListSources& command);
		void run(const Commands::RefreshHubDirectory& command);

		Events::HubListSources hubListSources(uint64_t request) const;
		Events::HubDirectory hubDirectory(uint64_t request) const;
		void run(const Commands::MeasureReachability& command);
		void run(const Commands::LookupPublicAddress& command);
		void run(const Commands::ReplayReachability& command);
		void run(const Commands::AttachFile& command);
		void run(const Commands::AttachPasted& command);
		void run(const Commands::FetchAttachment& command);
		void run(const Commands::SetChatMediaFetch& command);

		/** Decide about each picture a rich text message on 'hub' shows; 'sender' may be empty. */
		void chatMediaIn(HubHandle hub, const std::string& hubUrl, const std::string& sender,
		                 const std::string& text);

		/** Queue 'file' unless it is queued already, and say so. */
		void fetchMedia(HubHandle hub, const ChatAttachment& file, const std::string& hubUrl,
		                const std::string& sender);

		/** Where a file with this hash is on this disk, offered or downloaded; empty where it is not. */
		std::string localCopy(const std::string& tth) const;

		std::string downloadDirectory() const;

		/** Where pasted pictures are kept; empty for a core with no home. */
		std::string mediaFolder() const;
		void run(const Commands::RequestFilelist& command);
		void run(const Commands::Search& command);
		void run(const Commands::ForgetSearch& command);
		void run(const Commands::Enqueue& command);
		void run(const Commands::EnqueueFromSearch& command);
		void run(const Commands::EnqueueForReading& command);
		void run(const Commands::AddFallbackSource& command);
		void run(const Commands::SetPriority& command);
		void run(const Commands::RemoveFromQueue& command);
		void run(const Commands::SnapshotTransfers& command);
		void run(const Commands::ReportShare& command);
		void run(const Commands::AddShareFolder& command);
		void run(const Commands::RemoveShareFolder& command);
		void run(const Commands::RenameShareFolder& command);

		Events::ShareFolders shareFolders(uint64_t request) const;

		/** Answer a change to the folders, made or refused. */
		void foldersAnswered(Share::FolderRefusal refusal, const std::string& path,
		                     uint64_t request);
		void run(const Commands::OpenHub& command);
		void run(const Commands::CloseHub& command);
		void run(const Commands::ConnectHub& command);
		void run(const Commands::DisconnectHub& command);
		void run(const Commands::SetHubPolicy& command);
		void run(const Commands::SendChat& command);
		void run(const Commands::SendPrivateChat& command);
		void run(const Commands::SendPassword& command);
		void run(const Commands::RequestHistory& command);
		void run(const Commands::SubscribeBoard& command);
		void run(const Commands::UnsubscribeBoard& command);
		void run(const Commands::RequestBoardEntry& command);
		void run(const Commands::SubmitPost& command);
		void run(const Commands::WithdrawPost& command);

		void run(const Commands::OpenBoardStore& command);
		void run(const Commands::CloseBoardStore& command);
		void run(const Commands::StoreBoardEntries& command);
		void run(const Commands::DescribeBoard& command);
		void run(const Commands::QueryBoardState& command);
		void run(const Commands::LoadBoard& command);
		void run(const Commands::ResumeBoard& command);
		void run(const Commands::MarkRead& command);
		void run(const Commands::ServePosts& command);
		void run(const Commands::QueryPost& command);
		void run(const Commands::AdoptPost& command);
		void run(const Commands::EvictPosts& command);
		void run(const Commands::ReleasePosts& command);
		void run(const Commands::SetSetting& command);
		void run(const Commands::ReplaceGroup& command);
		void run(const Commands::SaveSettings& command);
		void run(const Commands::SetConnectionMode& command);
		void run(const Commands::ReportState& command);
		void run(const Commands::AddFavouriteHub& command);
		void run(const Commands::RemoveFavouriteHub& command);
		void run(const Commands::ForgetRecentHub& command);
		void run(const Commands::ClearRecentHubs& command);
		void run(const Commands::ForgetCompletedDownload& command);
		void run(const Commands::ClearCompletedDownloads& command);

		Events::FavouriteHubs favouriteHubs(uint64_t request) const;
		Events::RecentHubs recentHubs(uint64_t request) const;
		Events::CompletedDownloads completedDownloads(uint64_t request) const;

		/** What a hub opened as 'url' calls itself. */
		void hubNamed(const std::string& url, const std::string& name);

		Events::ConnectionMode connectionMode(uint64_t request) const;

		/** Tell what the command just carried out changed of the state ReportState tells. */
		void announceState();

		void EventSettingChanged(const Preferences& store, const std::string& group,
		                         const std::string& key, const std::string* value) override;

		Preferences* preferences(SettingsStore store) const;

		BoardStore* boardStore(BoardStoreHandle store);

		/** The open session 'hub' names, or null. */
		HubSession* session(HubHandle hub);

		/** Whether there is a server to ask; publishes why not where there is not. */
		bool haveReachabilityServer();

		void EventDropCheckStarted(bool ipv6) override;
		void EventDropCheckFinished(const DropVerdict& verdict) override;
		void EventDropCheckFailed(const char* reason) override;

		void EventAttachmentReady(const char* path, const char* tth, uint64_t size) override;
		void EventAttachmentFailed(const char* path, const char* reason) override;

		void EventFilelistFinished(const FilelistRequest* request) override;
		void EventQueueFileAdded(const QueueFile* file) override;
		void EventQueueFileRemoved(const QueueFile* file) override;
		void EventQueueFileVerified(const QueueFile* file) override;

		void EventTransferStarted(QuickDC::Transfer* transfer) override;
		void EventTransferFinished(QuickDC::Transfer* transfer) override;
		void EventTransferStopped(QuickDC::Transfer* transfer) override;
		void EventTransferError(QuickDC::Transfer* transfer) override;

		Events::ShareStatus shareStatus(uint64_t request) const;

		Core& core;
		ListenerList<EventSink> sinks;
		Subscription dropObserved;
		Subscription queueObserved;
		Subscription transfersObserved;
		Subscription configObserved;
		Subscription sessionObserved;

		/** The write being carried out, which the changes it makes are attributed to. */
		uint64_t writing = 0;

		/** A setting the connection mode is made of has changed since it was last told. */
		bool connectionModeChanged = false;
		std::map<std::string, std::unique_ptr<HubListFetch>> hublists;

		std::map<BoardStoreHandle, std::unique_ptr<BoardStore>> boardStores;

		struct MediaFetch {
			HubHandle hub = 0;
			std::string name;
		};

		/** Files fetched because a message named them, by hash, to be offered again once here. */
		std::map<std::string, MediaFetch> mediaFetches;

		/** The searches front ends sent, which results are told as answers to. */
		Share::SearchManager searches;

		/* Null without a configuration to keep them in. */
		std::unique_ptr<QuickDC::FavouriteHubs> favourites;
		std::unique_ptr<QuickDC::RecentHubs> recents;
		std::unique_ptr<QuickDC::CompletedDownloads> completed;
		std::unique_ptr<QuickDC::HubDirectory> directory;

		/* Last, so the sessions go before the subscriptions they report through. */
		std::map<HubHandle, std::unique_ptr<HubSession>> hubs;
};

}
}

#endif // HAVE_QUICKDC_API_SERVICE_H
