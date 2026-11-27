/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_UPDATE_FEED_H
#define HAVE_QUICKDC_UPDATE_FEED_H

#include <samurai/crypto/ed25519.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/** The largest package a feed may name: well above the 75 MB the largest is now. */
constexpr uint64_t QUICKDC_UPDATE_MAX_PACKAGE = 512ull * 1024 * 1024;

/** The largest feed document read. */
constexpr size_t QUICKDC_UPDATE_MAX_FEED = 16 * 1024;

namespace QuickDC {
namespace Update {

/** One channel's offer for one platform; see docs/distribution.md, "The feed, as built". */
struct Feed {
	std::string channel;
	std::string platform;
	std::string version;
	std::string url;
	uint64_t size = 0;
	/** Lowercase hex, 64 digits. */
	std::string sha256;
	Samurai::Crypto::Ed25519Signature signature {};
};

/** The feed in 'json', or nothing with 'why' saying what is wrong with it. Not verified. */
std::optional<Feed> parseFeed(std::string_view json, std::string& why);

/** What the signature covers. */
std::string signedText(const Feed& feed);

/** Whether 'key' signed this feed, as it stands. */
bool verifyFeed(const Feed& feed, const Samurai::Crypto::Ed25519PublicKey& key);

/** The feed as a document again, signature included: what a signing tool writes. */
std::string feedJson(const Feed& feed);

/** 'bytes' as lowercase hex. */
std::string toHex(const uint8_t* bytes, size_t length);

/** Hex of exactly 'length' bytes into 'out'; false for anything else. */
bool fromHex(std::string_view hex, uint8_t* out, size_t length);

}
}

#endif // HAVE_QUICKDC_UPDATE_FEED_H
