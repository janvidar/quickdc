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
 *                 Slots (3), Chunk size (1 MiB), Max downloads (3),
 *                 Keep alive interval (120) - seconds an ADC connection may sit
 *                 silent before a newline is sent over it, zero to send none.
 *                 Force active mode - keep announcing that a peer told to connect
 *                 here will get through, whatever the evidence says. Nothing can
 *                 prove that claim before somebody has acted on it, so this is how
 *                 a user who knows their gateway forwards the port says so instead
 *                 of waiting to be proven right by a peer that arrives.
 *                 UseUPnP - ask the internet gateway to forward the peer port at
 *                 start-up. Off unless asked for: a search tells every host on the
 *                 local segment that this one is here and what it runs, which is a
 *                 disclosure a user should choose.
 *                 Reachability server - the base host name of a DROP server
 *                 (Diagnostic Reachability & Obstacle Probe), "drop.quickdc.org"
 *                 and the like. When set, the core asks that server at start-up
 *                 what public address it sees this client at and whether it can
 *                 open a connection back to the port this client listens on, which
 *                 is the only way reachability is established rather than guessed.
 *                 The family's own name is prefixed ("https://ipv4." + the value),
 *                 because one connection has one source address and the dual-stack
 *                 name would measure whichever family that connection happened to
 *                 use rather than the one being asked about. Empty unless asked
 *                 for, on the same terms as UseUPnP beside it: asking tells that
 *                 server this client's address and the port it listens on, and has
 *                 it connect back to prove the answer, which is a disclosure a
 *                 user should choose.
 *   [Shares]      Total shares, Share 0 .. Share N - read at start-up, and
 *                 changed while running by the share folder commands, which a
 *                 front end uses rather than writing these keys.
 *   [Favourites], [Recent hubs], [Completed] - kept by the core; see
 *                 config/userlists.h and the commands that change them.
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
 * will act on it. One setting a user interface is likely to want is in that
 * position today: there is no global transfer rate limit (rates live in
 * Samurai::IO::Net::BandwidthManager, which only reports).
 *
 * Not thread safe. Belongs to the thread that constructed the Core.
 */

#include "quickdc.h"

#include "config/preferences.h"

#endif // HAVE_QUICKDC_CONFIG_API_H
