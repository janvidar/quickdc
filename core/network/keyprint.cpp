/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/keyprint.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include <samurai/io/net/socket.h>
#include <samurai/io/net/tlsfactory.h>
#include <samurai/util/base32.h>

#define KEYPRINT_HASH   "SHA256/"
#define KEYPRINT_DIGITS 52 /* base32 of 32 bytes, unpadded */

static bool is_base32(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
}

bool QuickDC::Keyprint::isValid(const char* keyprint)
{
	if (!keyprint) return false;

	const size_t prefix = strlen(KEYPRINT_HASH);

	if (strncasecmp(keyprint, KEYPRINT_HASH, prefix) != 0) return false;
	if (strlen(keyprint) != prefix + KEYPRINT_DIGITS) return false;

	for (const char* p = &keyprint[prefix]; *p; p++)
		if (!is_base32((char) toupper((unsigned char) *p))) return false;

	return true;
}

bool QuickDC::Keyprint::equals(const char* a, const char* b)
{
	if (!isValid(a) || !isValid(b)) return false;
	return strcasecmp(a, b) == 0;
}

bool QuickDC::Keyprint::fromQuery(const std::string& query, std::string& keyprint)
{
	size_t at = 0;

	while (at < query.size())
	{
		size_t end = query.find('&', at);
		if (end == std::string::npos) end = query.size();

		std::string param = query.substr(at, end - at);

		if (param.size() > 3 && param[2] == '=' && strncasecmp(param.c_str(), "kp", 2) == 0)
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
	base32_encode(digest, Samurai::IO::Net::TlsFactory::SHA256_LENGTH, encoded);

	keyprint = KEYPRINT_HASH;
	keyprint += encoded;
}

bool QuickDC::Keyprint::ofPeer(Samurai::IO::Net::Socket* socket, std::string& keyprint)
{
	if (!socket) return false;

	uint8_t digest[Samurai::IO::Net::TlsFactory::SHA256_LENGTH];
	if (!socket->TLSgetPeerCertificateSHA256(digest, sizeof(digest))) return false;

	format(digest, keyprint);
	return true;
}

bool QuickDC::Keyprint::ofOwnCertificate(std::string& keyprint)
{
#ifdef SSL_SUPPORT
	uint8_t digest[Samurai::IO::Net::TlsFactory::SHA256_LENGTH];
	if (!Samurai::IO::Net::TlsFactory::getOwnCertificateSHA256(digest, sizeof(digest)))
		return false;

	format(digest, keyprint);
	return true;
#else
	(void) keyprint;
	return false;
#endif
}
