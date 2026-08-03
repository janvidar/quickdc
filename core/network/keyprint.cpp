/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/keyprint.h"

#include <ctype.h>

#include <samurai/io/net/socket.h>
#include <samurai/io/net/tlsfactory.h>
#include <samurai/util/base32.h>
#include <span>
#include <string_view>

#define KEYPRINT_HASH   "SHA256/"
#define KEYPRINT_DIGITS 52 /* base32 of 32 bytes, unpadded */

static bool is_base32(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
}

bool QuickDC::Keyprint::isValid(const char* keyprint)
{
	if (!keyprint) return false;

	constexpr std::string_view hash(KEYPRINT_HASH);
	const std::string_view kp(keyprint);

	if (!Samurai::Util::istarts_with(kp, hash)) return false;
	if (kp.size() != hash.size() + KEYPRINT_DIGITS) return false;

	for (const char c : kp.substr(hash.size()))
		if (!is_base32(static_cast<char>(toupper(static_cast<unsigned char>(c))))) return false;

	return true;
}

bool QuickDC::Keyprint::equals(const char* a, const char* b)
{
	if (!isValid(a) || !isValid(b)) return false;
	return Samurai::Util::iequals(a, b);
}

bool QuickDC::Keyprint::fromQuery(const std::string& query, std::string& keyprint)
{
	size_t at = 0;

	while (at < query.size())
	{
		size_t end = query.find('&', at);
		if (end == std::string::npos) end = query.size();

		std::string param = query.substr(at, end - at);

		if (param.size() > 3 && param[2] == '=' && Samurai::Util::istarts_with(param, "kp"))
		{
			keyprint = param.substr(3);
			return true;
		}

		at = end + 1;
	}

	return false;
}

void QuickDC::Keyprint::format(const uint8_t* digest, std::string& keyprint)
{
	char encoded[KEYPRINT_DIGITS + 1];
	Samurai::Util::base32_encode(std::span<const unsigned char>(digest, Samurai::IO::Net::TlsFactory::SHA256_LENGTH), std::span<char>(encoded, sizeof(encoded)));

	keyprint = KEYPRINT_HASH;
	keyprint += encoded;
}

bool QuickDC::Keyprint::ofPeer(Samurai::IO::Net::Socket* socket, std::string& keyprint)
{
	if (!socket) return false;

	const auto digest = socket->TLSgetPeerCertificateSHA256();
	if (!digest) return false;

	format(digest->data(), keyprint);
	return true;
}

bool QuickDC::Keyprint::ofOwnCertificate(std::string& keyprint)
{
	const auto digest = Samurai::IO::Net::TlsFactory::getOwnCertificateSHA256();
	if (!digest) return false;

	format(digest->data(), keyprint);
	return true;
}
