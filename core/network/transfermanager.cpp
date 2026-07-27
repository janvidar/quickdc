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
	// transfers.push_back(transfer);
	QDBG("QuickDC::TransferManager::remove()");
	/*
	 * Compared against end(). A transfer that is not in the vector used to run
	 * off the end here, and erase() then memmoved an out of range interval.
	 */
	for (std::vector<QuickDC::Transfer*>::iterator it = transfers.begin(); it != transfers.end(); it++) {
		if (*it == transfer) {
			transfers.erase(it);
			return;
		}
	}
}

void QuickDC::TransferManager::onTransferStarted(Transfer*) {
	// do nothing?
}

void QuickDC::TransferManager::onTransferStopped(Transfer* xfer) {
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
		session->onTransferStopped();
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




