/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_TRANSFERMANAGER_H
#define HAVE_QUICKDC_TRANSFERMANAGER_H

#include "quickdc.h"

namespace QuickDC {

class Transfer;

class TransferListener {
	public:
		virtual ~TransferListener() { }
		
		/**
		 * Transfer was started. 
		 * Emitted once.
		 */
		virtual void EventTransferStarted(Transfer*);
		
		/**
		 * Transfer was stopped. Because the connection
		 * was closed, or this was requested via protocol.
		 */
		virtual void EventTransferStopped(Transfer*);
		
		/**
		 * Transfer was stopped due to an unexpected error.
		 */
		virtual void EventTransferError(Transfer*);
		
		/**
		 * Transfer completed.
		 */
		virtual void EventTransferFinished(Transfer*);
		
		/**
		 * Transfer progress update.
		 */
		virtual void EventTransferProgress(Transfer*);
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
		
	public:
		void add(Transfer*);
		void remove(Transfer*);
		
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
		bool EventMessage(const Samurai::Message*);
	
	protected:
		std::vector<Transfer*> transfers;
		std::vector<Transfer*> pendingRemove;
		std::vector<Transfer*>::iterator transferIterator;
};

}

#endif // HAVE_QUICKDC_TRANSFERMANAGER_H
