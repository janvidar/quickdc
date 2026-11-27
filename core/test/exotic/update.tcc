/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "update/feed.h"
#include "update/keys.h"
#include "update/version.h"

#include <samurai/crypto/ed25519.h>

#include <string>

using QuickDC::Update::Feed;
using QuickDC::Update::isNewer;
using QuickDC::Update::parseVersion;

namespace {

bool newer(const char* offered, const char* running)
{
	const auto a = parseVersion(offered);
	const auto b = parseVersion(running);
	return a && b && isNewer(*a, *b);
}

/* The test channel's seed, as committed in cli/tests/updates/test.seed. */
const char* const TEST_SEED = "ee3127d1cd9b92374624bb6428c19f2027899b8af0d2db140507786d10dc0be2";

Feed sampleFeed()
{
	Feed feed;
	feed.channel = "test";
	feed.platform = "linux-x86_64";
	feed.version = "0.1.1~nightly20261128.1a2b3c4";
	feed.url = "http://127.0.0.1:20000/QuickDC.AppImage";
	feed.size = 1234;
	feed.sha256 = std::string(64, 'a');
	return feed;
}

Feed signedFeed()
{
	Feed feed = sampleFeed();
	Samurai::Crypto::Ed25519Seed seed {};
	QuickDC::Update::fromHex(TEST_SEED, seed.data(), seed.size());
	feed.signature = *Samurai::Crypto::ed25519Sign(seed, QuickDC::Update::signedText(feed));
	return feed;
}

}

EXO_TEST(update_versions_of_each_kind_are_read,
{
	const auto release = parseVersion("1.2.3");
	const auto nightly = parseVersion("1.2.3~nightly20261127.246c20d");
	const auto local = parseVersion("1.2.3+246c20d-dirty");
	return release && !release->nightly && release->major == 1 && release->patch == 3
		&& nightly && nightly->nightly && nightly->date == 20261127 && nightly->commit == "246c20d"
		&& local && local->local;
});

EXO_TEST(update_what_is_not_a_version_is_refused,
{
	for (const char* bad : {"", "1.2", "1.2.3.4", "a.b.c", "1.2.3~nightly2026", "1.2.3~nightly20261127",
	                        "1.2.3~nightly20261127.", "1.2.3+", "1.2.3 ", "v1.2.3"})
		if (parseVersion(bad)) return false;
	return true;
});

EXO_TEST(update_numbers_are_ordered_as_numbers,
{
	return newer("0.1.10", "0.1.9") && newer("1.0.0", "0.9.9") && !newer("0.1.9", "0.1.10");
});

EXO_TEST(update_a_nightly_comes_before_its_release,
{
	return newer("0.2.0", "0.2.0~nightly20261201.abc") && !newer("0.2.0~nightly20261201.abc", "0.2.0")
		&& newer("0.2.1~nightly20261201.abc", "0.2.0");
});

EXO_TEST(update_nightlies_follow_their_dates,
{
	return newer("0.2.0~nightly20261202.bbb", "0.2.0~nightly20261201.aaa")
		&& !newer("0.2.0~nightly20261201.bbb", "0.2.0~nightly20261201.aaa");
});

EXO_TEST(update_the_same_version_is_not_newer,
{
	return !newer("0.2.0", "0.2.0") && !newer("0.2.0~nightly20261201.a", "0.2.0~nightly20261201.a");
});

EXO_TEST(update_a_signed_feed_verifies_and_reads_back,
{
	const Feed feed = signedFeed();
	const auto key = QuickDC::Update::channelKey("test");
	EXO_ASSERT(key);
	EXO_ASSERT(QuickDC::Update::verifyFeed(feed, *key));

	std::string why;
	const auto parsed = QuickDC::Update::parseFeed(QuickDC::Update::feedJson(feed), why);
	EXO_ASSERT(parsed);
	EXO_ASSERT(QuickDC::Update::verifyFeed(*parsed, *key));
	return parsed->version == feed.version && parsed->size == feed.size && parsed->url == feed.url;
});

EXO_TEST(update_any_field_changed_breaks_the_signature,
{
	const auto key = *QuickDC::Update::channelKey("test");
	const Feed good = signedFeed();
	Feed changed[7] = {good, good, good, good, good, good, good};
	changed[0].channel = "stable";
	changed[1].platform = "windows-x86_64";
	changed[2].version = "9.9.9";
	changed[3].url += "x";
	changed[4].size += 1;
	changed[5].sha256[0] = 'b';
	changed[6].signature[0] ^= 1;
	for (const Feed& feed : changed)
		if (QuickDC::Update::verifyFeed(feed, key)) return false;
	return true;
});

EXO_TEST(update_channels_without_a_key_have_none,
{
	return !QuickDC::Update::channelKey("no-such-channel") && QuickDC::Update::channelKey("test");
});

EXO_TEST(update_a_malformed_feed_is_refused,
{
	std::string why;
	const std::string good = QuickDC::Update::feedJson(signedFeed());
	EXO_ASSERT(QuickDC::Update::parseFeed(good, why));

	const auto without = [&](const std::string& from, const std::string& to) {
		std::string text = good;
		const size_t at = text.find(from);
		if (at != std::string::npos) text.replace(at, from.size(), to);
		return text;
	};

	for (const std::string& bad : {
		std::string("[]"), std::string("not json"),
		without("\"format\": 1", "\"format\": 2"),
		without("\"size\": 1234", "\"size\": 0"),
		without("\"size\": 1234", "\"size\": -1"),
		without(std::string(64, 'a'), std::string(63, 'a')),
		without(std::string(64, 'a'), std::string(64, 'A')),
		without("\"channel\": \"test\"", "\"channel\": \"\""),
		without("linux-x86_64", "linux\\nplatform windows"),
		std::string(QUICKDC_UPDATE_MAX_FEED + 1, ' ')})
	{
		if (QuickDC::Update::parseFeed(bad, why)) return false;
		if (why.empty()) return false;
	}
	return true;
});
