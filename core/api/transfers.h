/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_TRANSFERS_API_H
#define HAVE_QUICKDC_TRANSFERS_API_H

/**
 * The transfer engine, as a client sees it.
 *
 * Everything named here is declared elsewhere under core/; this header only
 * says which of it a client may rely on. That is worth a header of its own
 * because a user interface is expected to include nothing but core/api/, and
 * without one it would have to reach into core/network/ and would then be
 * coupled to every other declaration that happens to live there.
 *
 * What a transfer view needs, and where it comes from:
 *
 *   TransferManager   what is moving right now, with TransferListener to be
 *                     told as transfers come and go.
 *   Transfer          byte counts and a rate estimate for one connection.
 *   QueueManager      what we still want, with QueueStatus as a flat snapshot
 *                     of one entry and FileSource as somewhere to get it from.
 *
 * Two gaps worth knowing about before building against this:
 *
 *  - TransferListener::EventTransferProgress is declared but never emitted, so
 *    a live rate or progress column has to poll TransferManager::first() and
 *    next() rather than wait to be told.
 *  - a completed download leaves the queue and is not recorded anywhere, so
 *    there is no history to show.
 *
 * Queueing a file is not the same as looking for it. QueueManager searches for
 * sources one file per QUICKDC_SOURCE_SEARCH_INTERVAL across the whole queue,
 * which is a background rate and nothing else: anything a user is sitting and
 * waiting for wants QueueManager::requestSourceSearch() as well, and a source
 * that is already named - the author of a bulletin board post, say - wants
 * addKnownSource() rather than a search at all.
 *
 * None of it is thread safe. All of it belongs to the thread that constructed
 * the Core, and Core::run() is the only thing that advances it.
 */

#include "quickdc.h"

#include "network/downloadqueue.h"
#include "network/filesource.h"
#include "network/transfer.h"
#include "network/transfermanager.h"

#endif // HAVE_QUICKDC_TRANSFERS_API_H
