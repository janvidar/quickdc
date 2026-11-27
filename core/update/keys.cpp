/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "update/keys.h"
#include "update/feed.h"

namespace {

struct ChannelKey {
	std::string_view channel;
	/** Hex; empty until the channel has a key. */
	std::string_view key;
};

constexpr ChannelKey KEYS[] = {
	{"stable",  ""},
	{"nightly", ""},
	{"test",    "53cc16307681b1159f0c1e1eea3061cf0ac68a67b97e5c1582c965f6ccc879c1"},
};

}

std::optional<Samurai::Crypto::Ed25519PublicKey> QuickDC::Update::channelKey(std::string_view channel)
{
	for (const ChannelKey& known : KEYS)
	{
		if (known.channel != channel || known.key.empty()) continue;
		Samurai::Crypto::Ed25519PublicKey key {};
		if (!fromHex(known.key, key.data(), key.size())) return std::nullopt;
		return key;
	}
	return std::nullopt;
}
