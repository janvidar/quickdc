/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_TRANSFERMANAGER_H
#define HAVE_QUICKDC_TRANSFERMANAGER_H

#include "quickdc.h"

namespace QuickDC {

class Transfer;

/**
 * Implement this and register it with TransferManager::addListener() to be
 * told about transfers as they come and go. Intended for user interfaces.
 *
 * The bodies are empty rather than pure so that a listener only has to
 * override the events it cares about.
 */
class TransferListener {
	public:
		virtual ~TransferListener() { }

		/**
		 * Transfer was started.
		 * Emitted once.
		 */
		virtual void EventTransferStarted(Transfer*) { }

		/**
		 * Transfer was stopped. Because the connection
		 * was closed, or this was requested via protocol.
		 */
		virtual void EventTransferStopped(Transfer*) { }

		/**
		 * Transfer was stopped due to an unexpected error.
		 */
		virtual void EventTransferError(Transfer*) { }

		/**
		 * Transfer completed.
		 */
		virtual void EventTransferFinished(Transfer*) { }

		/**
		 * Transfer progress update.
		 */
		virtual void EventTransferProgress(Transfer*) { }
};

/**
 * The transfer manager, will coordinate all transfers,
 * limit transfer rates, and interface with the user interface.
 */
class TransferManager
	: public Samurai::MessageListener
{
	public:
		TransferManager();
		virtual ~TransferManager();

		TransferManager(const TransferManager&) = delete;
		TransferManager& operator=(const TransferManager&) = delete;
		
	public:
		void add(Transfer*);
		void remove(Transfer*);

		/** Listeners are not owned; remove them before they are destroyed. */
		void addListener(TransferListener*);
		void removeListener(TransferListener*);

		void onTransferStarted(Transfer*);
		void onTransferStopped(Transfer*);
		void onTransferError(Transfer*);
		void onTransferFinished(Transfer*);
		
		size_t size();
		
		size_t countUploads();
		
		/**
		 * Process transfer jobs.
		 */
		void process();
		
		/* Built-in iterators */
		Transfer* first();
		Transfer* next();
	
	protected:
		bool EventMessage(const Samurai::Message*) override;
	
	protected:
		std::vector<Transfer*> transfers;
		std::vector<Transfer*> pendingRemove;
		std::vector<Transfer*>::iterator transferIterator;
		std::vector<TransferListener*> listeners;
};

}

#endif // HAVE_QUICKDC_TRANSFERMANAGER_H
