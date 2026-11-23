/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include "network/clientsession.h"
#include "network/transfer.h"
#include "network/transfermanager.h"
#include <vector>

QuickDC::TransferManager::TransferManager() {
}

QuickDC::TransferManager::~TransferManager() {
	
}
		

void QuickDC::TransferManager::add(QuickDC::Transfer* transfer) {
	transfers.push_back(transfer);
	// QDBG("QuickDC::TransferManager::add(), transfers now: %i", transfers.size());
}

void QuickDC::TransferManager::remove(QuickDC::Transfer* transfer) {
	std::erase(transfers, transfer);

	/*
	 * And out of the announcements still to be made, which is the dangerous one.
	 * A transfer that has stopped waits in there for process() to tell its
	 * session, and it is only the session that then deletes it - but not every
	 * deletion comes from there. A connection dropping in the same turn as a
	 * chunk finished has the session release the transfer on the spot, and the
	 * entry left behind was dereferenced by the next process() to name a
	 * transfer that had already been freed.
	 *
	 * Every occurrence goes, so nothing can be left naming it whatever put it
	 * there.
	 */
	std::erase(pendingRemove, transfer);
}

QuickDC::Subscription QuickDC::TransferManager::subscribe(TransferListener* listener) {
	return listeners.subscribe(listener);
}

void QuickDC::TransferManager::addListener(TransferListener* listener) {
	listeners.add(listener);
}

void QuickDC::TransferManager::removeListener(TransferListener* listener) {
	listeners.remove(listener);
}

void QuickDC::TransferManager::onTransferStarted(Transfer* xfer) {
	listeners.notify([xfer](TransferListener& l) { l.EventTransferStarted(xfer); });
}

void QuickDC::TransferManager::onTransferStopped(Transfer* xfer) {
	/*
	 * Announced here rather than from process(), because by the time the
	 * pending list is drained the owning session has already been told and the
	 * transfer is about to be deleted - the byte counts and the rate would be
	 * gone. Listeners only read the transfer, so reporting from inside
	 * Transfer::exec() is safe; deleting it is what has to wait.
	 */
	listeners.notify([xfer](TransferListener& l) {
		if (xfer->isFinished())
			l.EventTransferFinished(xfer);
		else if (xfer->getStatus() != Transfer::OK)
			l.EventTransferError(xfer);
		else
			l.EventTransferStopped(xfer);
	});

	pendingRemove.push_back(xfer);
}

size_t QuickDC::TransferManager::size() {
	return transfers.size();
}

/**
 * Remove pending transfers
 */
void QuickDC::TransferManager::process() {
	while (pendingRemove.size()) {
		Transfer* xfer = pendingRemove.back();
		pendingRemove.pop_back();
		if (!xfer) continue;

		PrivateTransferListener* session = xfer->getPrivateTransferListener();
		if (!session) continue;

		/*
		 * Named, so that a session holding more than one transfer over the life of
		 * its connection releases the one that stopped rather than whichever it
		 * happens to be holding now.
		 */
		session->onTransferStopped(xfer);
	}
}

QuickDC::Transfer* QuickDC::TransferManager::first() {
	transferIterator = transfers.begin();
	if (transferIterator != transfers.end())
		return *transferIterator;
	return 0;
}

QuickDC::Transfer* QuickDC::TransferManager::next() {
	transferIterator++;
	if (transferIterator != transfers.end())
		return *transferIterator;
	return 0;
}

size_t QuickDC::TransferManager::countUploads() {
	size_t count = 0;
	/* 'xfer' was never reassigned, so this looped forever on the first entry. */
	for (QuickDC::Transfer* xfer = first(); xfer; xfer = next()) {
		if (xfer->isUpload()) count++;
	}
	return count;

}

bool QuickDC::TransferManager::EventMessage(const Samurai::Message* msg)
{
	if (msg->getID() == QuickDC::MsgTransferStopped)
	{
		process();
		return true;
	}
	return false;
}




