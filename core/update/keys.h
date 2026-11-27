/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_UPDATE_KEYS_H
#define HAVE_QUICKDC_UPDATE_KEYS_H

#include <samurai/crypto/ed25519.h>

#include <optional>
#include <string_view>

namespace QuickDC {
namespace Update {

/**
 * The key a channel's feed is signed with, or nothing for a channel that has
 * none yet, which then never updates.
 *
 * "test" is the test suite's: its private half is committed with the tests
 * (cli/tests/updates/test.seed), so it is trusted only from a feed on the
 * loopback - see Updater.
 */
std::optional<Samurai::Crypto::Ed25519PublicKey> channelKey(std::string_view channel);

}
}

#endif // HAVE_QUICKDC_UPDATE_KEYS_H
