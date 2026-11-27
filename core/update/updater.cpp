/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "update/updater.h"
#include "update/keys.h"
#include "update/version.h"

#include "config/preferences.h"
#include "config/settings.h"

#include <samurai/crypto/digest/sha.h>
#include <samurai/io/net/url.h>
#include <samurai/util/random.h>

#include <filesystem>
#include <fstream>
#include <system_error>

#ifndef QUICKDC_UPDATE_CHANNEL
#define QUICKDC_UPDATE_CHANNEL ""
#endif

namespace {

using Why = QuickDC::Api::Events::UpdateFailed::Why;

/* Between three and ten minutes after start: after the start has settled, and
   spread so that a release does not have every client ask at once. */
constexpr int FIRST_CHECK_MIN_S = 3 * 60;
constexpr int FIRST_CHECK_MAX_S = 10 * 60;

/* Then about daily, give or take an hour. */
constexpr int DAILY_S = 24 * 60 * 60;
constexpr int JITTER_S = 60 * 60;

Samurai::IO::Net::HTTP::Client::Options fetchOptions(bool package)
{
	Samurai::IO::Net::HTTP::Client::Options options;
	options.userAgent = PRODUCT "/" VERSION;
	options.maxRedirects = 5;
	if (package)
	{
		options.timeout = std::chrono::minutes(20);
		options.limits.maxBody = QUICKDC_UPDATE_MAX_PACKAGE;
		options.limits.maxChunkSize = QUICKDC_UPDATE_MAX_PACKAGE;
	}
	else
	{
		options.timeout = std::chrono::seconds(60);
		options.limits.maxBody = QUICKDC_UPDATE_MAX_FEED;
		options.limits.maxChunkSize = QUICKDC_UPDATE_MAX_FEED;
	}
	return options;
}

bool loopbackHost(const std::string& host)
{
	return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

/* Only these may be written into a file name. */
bool plainVersion(const std::string& version)
{
	return !version.empty() && version.find_first_not_of(
		"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.~+-") == std::string::npos;
}

std::string sha256Of(const std::string& bytes)
{
	Samurai::Crypto::Digest::Sha256 hash;
	hash.update(bytes.data(), bytes.size());
	const Samurai::Crypto::Digest::HashValue& digest = hash.digest();
	return QuickDC::Update::toHex(digest.getData(), digest.size());
}

}

const char* QuickDC::Update::platformName()
{
#if defined(__linux__) && defined(__x86_64__)
	return "linux-x86_64";
#elif defined(__APPLE__)
	return "macos-universal";
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	return "windows-x86_64";
#else
	return "";
#endif
}

const char* QuickDC::Update::buildChannel()
{
	return QUICKDC_UPDATE_CHANNEL;
}

QuickDC::Update::Updater::Updater(Samurai::EventLoop& loop_, const Preferences& config_, std::string home_,
                                  std::string running_, Publish publish_)
	: loop(loop_)
	, config(config_)
	, home(std::move(home_))
	, running(std::move(running_))
	, publish(std::move(publish_))
{
	if (home.empty()) return;
	schedule(std::chrono::seconds(Samurai::Util::pseudoRandom(FIRST_CHECK_MIN_S, FIRST_CHECK_MAX_S)));
}

QuickDC::Update::Updater::~Updater() = default;

std::string QuickDC::Update::Updater::directory() const
{
	return home.empty() ? std::string() : home + "/updates";
}

void QuickDC::Update::Updater::check()
{
	begin(true);
}

void QuickDC::Update::Updater::schedule(std::chrono::milliseconds after)
{
	/* Kept alive a while: this runs from inside the timer's own callback. */
	retiredTimer = std::move(timer);
	timer = std::make_unique<Samurai::Timer>(loop, static_cast<Samurai::TimerListener*>(this), after, true);
}

void QuickDC::Update::Updater::EventTimeout(Samurai::Timer*)
{
	begin(false);
}

void QuickDC::Update::Updater::begin(bool asked)
{
	/* A check already under way will be reported, so asking again says nothing. */
	if (stage != Stage::Idle) return;

	const auto quiet = [&](Why why, const char* reason) {
		if (asked) publish(Api::Events::UpdateFailed{why, reason});
		finish();
	};

	if (home.empty()) return quiet(Why::Storage, "this core has no home to keep an update in");
	if (!Settings::read(config, Settings::UpdatesEnabled)) return quiet(Why::Disabled, "updates are turned off");

	channel = Settings::read(config, Settings::UpdateChannel);
	if (channel.empty()) channel = buildChannel();
	if (channel.empty()) return quiet(Why::NoChannel, "this build was not packaged for a channel");

	if (!*platformName())
	{
		/* Not a failure of any one check, so not something to repeat daily either. */
		if (asked) publish(Api::Events::UpdateFailed{Why::NoPlatform, "there are no packages for this platform"});
		return;
	}

	if (!channelKey(channel))
	{
		fail(Why::NoKey, "the " + channel + " channel has no key to check a feed with");
		return;
	}

	std::string base = Settings::read(config, Settings::UpdateFeed);
	while (!base.empty() && base.back() == '/') base.pop_back();
	const std::string url = base + "/" + channel + "/" + platformName() + ".json";

	if (!allowed(url))
	{
		fail(Why::Invalid, "the feed address " + url + " is not https"
			+ (channel == "test" ? " on the loopback" : ""));
		return;
	}

	const Samurai::IO::Net::URL parsed(url);
	if (!parsed.isValid())
	{
		fail(Why::Invalid, "the feed address " + url + " is not a URL");
		return;
	}

	QDBG("Asking %s for an update", url.c_str());
	stage = Stage::Feed;
	offer.reset();
	fetch(fetchOptions(false), parsed);
}

void QuickDC::Update::Updater::fetch(const Samurai::IO::Net::HTTP::Client::Options& options,
                                     const Samurai::IO::Net::URL& url)
{
	/* Kept alive a while: the package is asked for from inside the feed's callback. */
	retiredClient = std::move(client);
	client = std::make_unique<Samurai::IO::Net::HTTP::Client>(
		loop, static_cast<Samurai::IO::Net::HTTP::RequestEventHandler*>(this), options);
	client->get(url);
}

bool QuickDC::Update::Updater::allowed(const std::string& url) const
{
	const Samurai::IO::Net::URL parsed(url);
	if (!parsed.isValid()) return false;

	/* The test key's private half is public, so only something already on this machine may use it. */
	if (channel == "test") return loopbackHost(parsed.getHostname());
	return parsed.getScheme() == "https";
}

void QuickDC::Update::Updater::fail(Why why, std::string reason)
{
	QERR("Update: %s", reason.c_str());
	publish(Api::Events::UpdateFailed{why, std::move(reason)});
	finish();
}

void QuickDC::Update::Updater::finish()
{
	stage = Stage::Idle;
	schedule(std::chrono::seconds(DAILY_S + Samurai::Util::pseudoRandom(-JITTER_S, JITTER_S)));
}

void QuickDC::Update::Updater::EventHttpResponse(Samurai::IO::Net::HTTP::Client*,
                                                 const Samurai::IO::Net::HTTP::Response& response)
{
	const int code = response.getStatusCode();
	if (code != 200)
	{
		fail(Why::Unreachable, std::string(stage == Stage::Feed ? "the feed" : "the package")
			+ " answered " + std::to_string(code));
		return;
	}

	if (stage == Stage::Feed) feedArrived(response.getBody());
	else if (stage == Stage::Package) packageArrived(response.getBody());
}

void QuickDC::Update::Updater::EventHttpError(Samurai::IO::Net::HTTP::Client*,
                                              Samurai::IO::Net::HTTP::Status, const char* msg)
{
	fail(Why::Unreachable, std::string(stage == Stage::Feed ? "the feed" : "the package")
		+ " could not be fetched: " + (msg && *msg ? msg : "the connection failed"));
}

void QuickDC::Update::Updater::feedArrived(const std::string& body)
{
	std::string why;
	std::optional<Feed> feed = parseFeed(body, why);
	if (!feed)
	{
		fail(Why::Invalid, why);
		return;
	}

	/* Verified before anything in it is believed, the channel and platform included. */
	const std::optional<Samurai::Crypto::Ed25519PublicKey> key = channelKey(channel);
	if (!key || !verifyFeed(*feed, *key))
	{
		fail(Why::BadSignature, "the feed's signature does not verify against the " + channel + " key");
		return;
	}

	if (feed->channel != channel || feed->platform != platformName())
	{
		fail(Why::WrongFeed, "the feed is for " + feed->channel + " on " + feed->platform
			+ ", not " + channel + " on " + platformName());
		return;
	}

	const std::optional<Version> offered = parseVersion(feed->version);
	const std::optional<Version> current = parseVersion(running);
	if (!offered || !plainVersion(feed->version))
	{
		fail(Why::Invalid, "the feed offers '" + feed->version + "', which is not a version");
		return;
	}

	if (current && !isNewer(*offered, *current))
	{
		publish(Api::Events::UpdateCurrent{running, feed->version});
		finish();
		return;
	}

	const std::string path = packagePath(*feed);
	if (path.empty())
	{
		fail(Why::Invalid, "the feed names a package of a kind this platform does not install");
		return;
	}

	if (!allowed(feed->url))
	{
		fail(Why::Invalid, "the package address " + feed->url + " is not https");
		return;
	}

	if (holds(path, *feed))
	{
		publish(Api::Events::UpdateReady{feed->version, path});
		finish();
		return;
	}

	const Samurai::IO::Net::URL parsed(feed->url);
	if (!parsed.isValid())
	{
		fail(Why::Invalid, "the package address " + feed->url + " is not a URL");
		return;
	}

	publish(Api::Events::UpdateAvailable{feed->version});
	offer = std::move(feed);
	stage = Stage::Package;
	fetch(fetchOptions(true), parsed);
}

void QuickDC::Update::Updater::packageArrived(const std::string& body)
{
	const Feed& feed = *offer;

	if (body.size() != feed.size)
	{
		fail(Why::Integrity, "the package is " + std::to_string(body.size()) + " bytes, not the "
			+ std::to_string(feed.size) + " the feed signed for");
		return;
	}

	if (sha256Of(body) != feed.sha256)
	{
		fail(Why::Integrity, "the package's SHA-256 is not the one the feed signed for");
		return;
	}

	const std::string path = packagePath(feed);
	const std::string partial = path + ".partial";
	std::error_code ec;
	std::filesystem::create_directories(directory(), ec);

	{
		std::ofstream out(partial, std::ios::binary | std::ios::trunc);
		out.write(body.data(), static_cast<std::streamsize>(body.size()));
		out.close();
		if (!out)
		{
			std::filesystem::remove(partial, ec);
			fail(Why::Storage, "the package could not be written to " + directory());
			return;
		}
	}

	std::filesystem::rename(partial, path, ec);
	if (ec)
	{
		std::filesystem::remove(partial, ec);
		fail(Why::Storage, "the package could not be kept as " + path);
		return;
	}

	sweep(path);
	publish(Api::Events::UpdateReady{feed.version, path});
	finish();
}

std::string QuickDC::Update::Updater::packagePath(const Feed& feed) const
{
	const std::string file = Samurai::IO::Net::URL(feed.url).getPath();
	std::string kind;
	for (const char* known : {".AppImage", ".exe", ".zip"})
	{
		const std::string suffix = known;
		if (file.size() >= suffix.size() && file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0)
			kind = suffix;
	}

#if defined(__linux__)
	const char* expected = ".AppImage";
#elif defined(__APPLE__)
	const char* expected = ".zip";
#else
	const char* expected = ".exe";
#endif
	if (kind != expected || !plainVersion(feed.version)) return std::string();
	return directory() + "/QuickDC-" + feed.version + "-" + feed.platform + kind;
}

bool QuickDC::Update::Updater::holds(const std::string& path, const Feed& feed)
{
	std::error_code ec;
	if (std::filesystem::file_size(path, ec) != feed.size || ec) return false;

	std::ifstream in(path, std::ios::binary);
	Samurai::Crypto::Digest::Sha256 hash;
	char chunk[65536];
	while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0)
		hash.update(chunk, static_cast<size_t>(in.gcount()));
	const Samurai::Crypto::Digest::HashValue& digest = hash.digest();
	return toHex(digest.getData(), digest.size()) == feed.sha256;
}

void QuickDC::Update::Updater::sweep(const std::string& keep) const
{
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(directory(), ec))
	{
		if (entry.path() == std::filesystem::path(keep)) continue;
		std::error_code ignored;
		std::filesystem::remove(entry.path(), ignored);
	}
}
