/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ATTACHMENTS_API_H
#define HAVE_QUICKDC_ATTACHMENTS_API_H

/**
 * Offering a file by its hash alone, without sharing it.
 *
 * A facade, for the same reason as api/transfers.h: a user interface includes
 * core/api/ and nothing else.
 *
 * This is what a chat attachment is made of. The user picks a file, the message
 * carries a magnet URI naming its tiger tree hash, and whoever has that URI can
 * fetch the file from us like any other upload. The file itself stays out of the
 * share: it appears in no file list, no search by name matches it, and the
 * number of files and bytes a hub is told about does not change. The hash is the
 * capability - having it is what grants access, and there is no other way to
 * find the file.
 *
 * How an attachment goes:
 *
 *  1. QuickDC::Share::Manager::attach(path, listener) takes the file. It returns
 *     false only when there is nothing to work with; anything else is reported
 *     to the listener.
 *  2. The file is hashed on the hash worker thread, so nothing blocks. A file
 *     that has been hashed before is answered out of the hash cache instead.
 *  3. EventAttachmentReady() hands over the hash to put in the magnet URI, on
 *     the thread that drives the Core - or EventAttachmentFailed() says why the
 *     file cannot be offered.
 *  4. A peer that asks for "TTH/<hash>" is served, because
 *     Manager::getFileByTTH() finds the file. A search by TTH finds it too.
 *
 * Worth knowing before building on this:
 *
 *  - attachments are remembered in the configuration under [Attachments], so a
 *    magnet URI in an old message keeps working across a restart. A file that
 *    has been edited since is dropped at startup instead: the remembered hash
 *    does not describe it any more, and serving the bytes under it would give
 *    the downloader nothing but a failed transfer.
 *  - there is no way to take one back while the client is running. An
 *    attachment lasts as long as the share manager, and past that.
 *  - the hash manager only logs a hash job that fails, and tells no listener, so
 *    a failure is noticed when hashing next falls idle rather than at the moment
 *    it happens.
 *
 * Not thread safe. Belongs to the thread that constructed the Core.
 */

#include "quickdc.h"

#include "share/sharemanager.h"

namespace QuickDC {
namespace Share {

/**
 * Implement this to hear about a file handed to Manager::attach().
 */
class AttachmentListener {
	public:
		virtual ~AttachmentListener() = default;

		/**
		 * @param path  the file, exactly as it was given to attach()
		 * @param tth   the tiger tree root, base32, as it goes into a magnet URI
		 * @param size  the file's length in bytes
		 */
		virtual void EventAttachmentReady(const char* path, const char* tth, uint64_t size) = 0;

		/** @param reason why it cannot be offered; for a person to read. */
		virtual void EventAttachmentFailed(const char* path, const char* reason) = 0;
};

}
}

#endif // HAVE_QUICKDC_ATTACHMENTS_API_H
