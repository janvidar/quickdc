/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * update-feed: write a signed update feed (docs/distribution.md, "The feed, as built").
 *
 *   update-feed --seed FILE --channel C --version V --url U --package FILE
 *               [--platform P] [--size N] [--sha256 HEX]
 *
 * FILE for --seed holds the Ed25519 private seed as 64 hex digits. The size and
 * SHA-256 are the package's unless given, which the tests use to make a feed that
 * lies about its package. The feed goes to standard output.
 */

#include "update/feed.h"
#include "update/updater.h"

#include <samurai/crypto/digest/sha.h>
#include <samurai/crypto/ed25519.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

namespace {

int usage()
{
	std::fprintf(stderr, "usage: update-feed --seed FILE --channel C --version V --url U --package FILE\n"
	                     "                   [--platform P] [--size N] [--sha256 HEX]\n");
	return 2;
}

bool readFile(const std::string& path, std::string& out)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}

}

int main(int argc, char** argv)
{
	std::map<std::string, std::string> given;
	for (int n = 1; n < argc; n++)
	{
		const std::string arg = argv[n];
		if (arg.compare(0, 2, "--") != 0 || n + 1 >= argc) return usage();
		given[arg.substr(2)] = argv[++n];
	}
	for (const char* needed : {"seed", "channel", "version", "url", "package"})
		if (!given.count(needed)) return usage();

	std::string seedText;
	Samurai::Crypto::Ed25519Seed seed {};
	if (!readFile(given["seed"], seedText)) { std::fprintf(stderr, "update-feed: cannot read %s\n", given["seed"].c_str()); return 1; }
	while (!seedText.empty() && (seedText.back() == '\n' || seedText.back() == '\r' || seedText.back() == ' ')) seedText.pop_back();
	if (!QuickDC::Update::fromHex(seedText, seed.data(), seed.size()))
	{
		std::fprintf(stderr, "update-feed: %s does not hold 64 lowercase hex digits\n", given["seed"].c_str());
		return 1;
	}

	std::string package;
	if (!readFile(given["package"], package)) { std::fprintf(stderr, "update-feed: cannot read %s\n", given["package"].c_str()); return 1; }

	QuickDC::Update::Feed feed;
	feed.channel = given["channel"];
	feed.platform = given.count("platform") ? given["platform"] : QuickDC::Update::platformName();
	feed.version = given["version"];
	feed.url = given["url"];
	feed.size = given.count("size") ? std::stoull(given["size"]) : package.size();

	if (given.count("sha256")) feed.sha256 = given["sha256"];
	else
	{
		Samurai::Crypto::Digest::Sha256 hash;
		hash.update(package.data(), package.size());
		const auto& digest = hash.digest();
		feed.sha256 = QuickDC::Update::toHex(digest.getData(), digest.size());
	}

	const auto signature = Samurai::Crypto::ed25519Sign(seed, QuickDC::Update::signedText(feed));
	if (!signature) { std::fprintf(stderr, "update-feed: the TLS library would not sign\n"); return 1; }
	feed.signature = *signature;

	std::fputs(QuickDC::Update::feedJson(feed).c_str(), stdout);
	return 0;
}
