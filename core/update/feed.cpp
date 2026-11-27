/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "update/feed.h"

#include <samurai/io/json.h>

#include <cstdio>

namespace {

/* A line feed would let a field forge a line of the signed text, so none may hold one. */
bool oneLine(const std::string& text)
{
	return !text.empty() && text.find_first_of("\r\n") == std::string::npos;
}

std::string jsonString(const std::string& text)
{
	std::string out = "\"";
	for (const char c : text)
	{
		if (c == '"' || c == '\\') { out += '\\'; out += c; }
		else if (static_cast<unsigned char>(c) < 0x20)
		{
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
			out += escaped;
		}
		else out += c;
	}
	return out + "\"";
}

}

std::string QuickDC::Update::toHex(const uint8_t* bytes, size_t length)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(length * 2);
	for (size_t n = 0; n < length; n++)
	{
		out += digits[bytes[n] >> 4];
		out += digits[bytes[n] & 0x0f];
	}
	return out;
}

bool QuickDC::Update::fromHex(std::string_view hex, uint8_t* out, size_t length)
{
	if (hex.size() != length * 2) return false;
	const auto digit = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		return -1;
	};
	for (size_t n = 0; n < length; n++)
	{
		const int high = digit(hex[2 * n]);
		const int low = digit(hex[2 * n + 1]);
		if (high < 0 || low < 0) return false;
		out[n] = static_cast<uint8_t>(high << 4 | low);
	}
	return true;
}

std::optional<QuickDC::Update::Feed> QuickDC::Update::parseFeed(std::string_view json, std::string& why)
{
	if (json.size() > QUICKDC_UPDATE_MAX_FEED)
	{
		why = "the feed is larger than any feed is";
		return std::nullopt;
	}

	Samurai::IO::JsonDocument document;
	if (document.parse(json) != Samurai::IO::JsonError::Ok || !document.getRoot() || !document.getRoot()->isObject())
	{
		why = "the feed is not a JSON object";
		return std::nullopt;
	}
	const Samurai::IO::JsonValue& root = *document.getRoot();

	if (root.findInteger("format", 0) != 1)
	{
		why = "the feed is in a format this version does not read";
		return std::nullopt;
	}

	Feed feed;
	feed.channel = root.findString("channel");
	feed.platform = root.findString("platform");
	feed.version = root.findString("version");
	feed.url = root.findString("url");
	feed.sha256 = root.findString("sha256");
	const std::string signature = root.findString("signature");
	const std::optional<int64_t> size = root.findOptionalInteger("size");

	for (const std::string* field : {&feed.channel, &feed.platform, &feed.version, &feed.url})
	{
		if (!oneLine(*field))
		{
			why = "the feed leaves out a field, or has one with a line break in it";
			return std::nullopt;
		}
	}

	uint8_t digest[32];
	if (!fromHex(feed.sha256, digest, sizeof(digest)))
	{
		why = "the feed's sha256 is not 64 lowercase hex digits";
		return std::nullopt;
	}

	if (!fromHex(signature, feed.signature.data(), feed.signature.size()))
	{
		why = "the feed's signature is not 128 lowercase hex digits";
		return std::nullopt;
	}

	if (!size || *size <= 0 || static_cast<uint64_t>(*size) > QUICKDC_UPDATE_MAX_PACKAGE)
	{
		why = "the feed names no size, or one no package has";
		return std::nullopt;
	}
	feed.size = static_cast<uint64_t>(*size);
	return feed;
}

std::string QuickDC::Update::signedText(const Feed& feed)
{
	return "quickdc-update 1\n"
		"channel " + feed.channel + "\n"
		"platform " + feed.platform + "\n"
		"version " + feed.version + "\n"
		"url " + feed.url + "\n"
		"size " + std::to_string(feed.size) + "\n"
		"sha256 " + feed.sha256 + "\n";
}

bool QuickDC::Update::verifyFeed(const Feed& feed, const Samurai::Crypto::Ed25519PublicKey& key)
{
	return Samurai::Crypto::ed25519Verify(key, signedText(feed), feed.signature);
}

std::string QuickDC::Update::feedJson(const Feed& feed)
{
	return "{\n"
		"  \"format\": 1,\n"
		"  \"channel\": " + jsonString(feed.channel) + ",\n"
		"  \"platform\": " + jsonString(feed.platform) + ",\n"
		"  \"version\": " + jsonString(feed.version) + ",\n"
		"  \"url\": " + jsonString(feed.url) + ",\n"
		"  \"size\": " + std::to_string(feed.size) + ",\n"
		"  \"sha256\": " + jsonString(feed.sha256) + ",\n"
		"  \"signature\": \"" + toHex(feed.signature.data(), feed.signature.size()) + "\"\n"
		"}\n";
}
