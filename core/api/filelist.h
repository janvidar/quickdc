/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_FILELIST_API_H
#define HAVE_QUICKDC_FILELIST_API_H

/**
 * Browsing another user's shared files.
 *
 * A facade, for the same reason as api/transfers.h: a user interface includes
 * core/api/ and nothing else.
 *
 * Note that the file list types live in a top level 'Share' namespace, not in
 * QuickDC::Share, which is where the local share manager lives. The two are
 * unrelated - Share::RemoteDir describes what someone else holds.
 *
 * How a browse goes:
 *
 *  1. QueueManager::requestFilelist(hubUrl, cid) queues the request and returns
 *     a FilelistRequest. Its getLocalFilename() is where the list will land.
 *  2. QueueManager::process() makes the connection, retrying a peer who is not
 *     reachable yet, so a request that cannot be served now is not lost.
 *  3. When the list arrives it is unpacked, moved into place, and the request is
 *     dropped from the pending set.
 *  4. Open getLocalFilename() with a Share::RemoteFileList and decode() it, then
 *     walk root.
 *
 * Step 3 is where this is awkward to build on: **nothing reports it**.
 * QueueManager::onFilelistDownloaded() is internal and returns only a count of
 * the new sources it found, and no listener is told. A user interface has to
 * poll countFilelistRequests() and getFilelistRequest(n) and notice a request it
 * was waiting for go away. A callback here would be worth adding.
 *
 * Not thread safe. Belongs to the thread that constructed the Core.
 */

#include "quickdc.h"

#include "network/downloadqueue.h"
#include "share/dcfilelist.h"

#endif // HAVE_QUICKDC_FILELIST_API_H
