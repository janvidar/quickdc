/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_SERVICE_H
#define HAVE_QUICKDC_API_SERVICE_H

#include "api/attachments.h"
#include "api/protocol.h"
#include "network/downloadqueue.h"
#include "network/transfermanager.h"
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
class Service : private DropCheckListener, private Share::AttachmentListener,
                private QueueListener, private TransferListener {
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
		class HubSession;
		class BoardStore;

		void run(const Commands::FetchHubList& command);
		void run(const Commands::MeasureReachability& command);
		void run(const Commands::LookupPublicAddress& command);
		void run(const Commands::ReplayReachability& command);
		void run(const Commands::ApplyPortForwarding& command);
		void run(const Commands::RepublishClientInfo& command);
		void run(const Commands::AttachFile& command);
		void run(const Commands::RequestFilelist& command);
		void run(const Commands::Search& command);
		void run(const Commands::Enqueue& command);
		void run(const Commands::EnqueueFromSearch& command);
		void run(const Commands::EnqueueForReading& command);
		void run(const Commands::AddFallbackSource& command);
		void run(const Commands::SetPriority& command);
		void run(const Commands::RemoveFromQueue& command);
		void run(const Commands::SnapshotTransfers& command);
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

		Core& core;
		ListenerList<EventSink> sinks;
		Subscription dropObserved;
		Subscription queueObserved;
		Subscription transfersObserved;
		std::map<std::string, std::unique_ptr<HubListFetch>> hublists;

		std::map<BoardStoreHandle, std::unique_ptr<BoardStore>> boardStores;

		/* Last, so the sessions go before the subscriptions they report through. */
		std::map<HubHandle, std::unique_ptr<HubSession>> hubs;
};

}
}

#endif // HAVE_QUICKDC_API_SERVICE_H
