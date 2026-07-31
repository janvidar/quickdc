/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONFIG_API_H
#define HAVE_QUICKDC_CONFIG_API_H

/**
 * The configuration store, as a client sees it.
 *
 * A facade over config/preferences.h, for the same reason as api/transfers.h:
 * a user interface includes core/api/ and nothing else.
 *
 * The Core holds two of these. Core::config is ~/.quickdc/quickdc.conf, the
 * settings below. Core::session is ~/.quickdc/session.conf, one group per hub
 * URL holding Nickname, Description, E-mail, Password, Last Connect and
 * Keyprint.
 *
 * Both share a mutable current group, so select it immediately before every
 * read or write and never rely on one set earlier - see the note on
 * Core::session in api/core.h for what goes wrong otherwise.
 *
 * The settings the core actually reads:
 *
 *   [User]        Nickname, Description, E-mail, Password, Client ID,
 *                 Private ID
 *   [Network]     Port (1411), Bind (0.0.0.0), Address, Active mode,
 *                 Slots (3), Chunk size (1 MiB), Max downloads (3)
 *   [Shares]      Total shares, Share 0 .. Share N
 *   [Chat media]  Fetch automatically, Maximum size, Keep pasted for days - all
 *                 read by the user interface, none by the core.
 *   [Attachments] Total, File 0 .. File N - files offered in chat by their hash
 *                 alone. Each entry is a tiger root, a space, and the path:
 *                 "VN6...RWZ /home/me/diagram.png". These are not shares - they
 *                 are in no file list and no name search matches them.
 *   [Downloads]   Directory
 *   [Local hub]   Enabled, Name, Description, Max users, Max history,
 *                 Max send queue bytes
 *   [HTTP server] Enabled
 *
 * A key not in that list can be stored and read back, but nothing in the core
 * will act on it. Two settings a user interface is likely to want are in that
 * position today: there is no global transfer rate limit (rates live in
 * Samurai::IO::Net::BandwidthManager, which only reports), and the shares are
 * read once at Share::Manager::initialize() with addShare() and removeShare()
 * protected, so a folder added here does not take effect until a restart.
 *
 * Not thread safe. Belongs to the thread that constructed the Core.
 */

#include "quickdc.h"

#include "config/preferences.h"

#endif // HAVE_QUICKDC_CONFIG_API_H
