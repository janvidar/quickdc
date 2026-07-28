/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_KEYPRINT_H
#define HAVE_QUICKDC_KEYPRINT_H

#include "quickdc.h"
#include <string>

namespace Samurai {
namespace IO {
namespace Net {
class Socket;
}
}
}

namespace QuickDC {

/**
 * A certificate keyprint, as the ADC KEYP extension writes it: a hash name, a
 * forward slash, and the base32 encoding of that hash of the peer's
 * certificate in DER form.
 *
 * SHA256 is the only hash the extension names, and a keyprint whose hash we
 * cannot compute is no better than no keyprint at all, so nothing else is
 * accepted here.
 */
class Keyprint
{
	public:
		static bool isValid(const char* keyprint);

		/**
		 * True if both are keyprints of the same certificate. Anything
		 * malformed compares unequal, so a keyprint we cannot read counts as
		 * a mismatch rather than as a match.
		 */
		static bool equals(const char* a, const char* b);

		/**
		 * The 'kp' parameter of a hub URL's query, as in
		 * "adcs://host:port/?kp=SHA256/{base32}".
		 *
		 * The value is returned as it was written and is not checked: telling
		 * "no keyprint given" from "a keyprint we cannot use" is the caller's
		 * business, because the two mean very different things.
		 */
		static bool fromQuery(const std::string& query, std::string& keyprint);

		/**
		 * The keyprint of the certificate the peer of 'socket' presented.
		 * False if it is not a TLS connection, or the peer sent none.
		 */
		static bool ofPeer(Samurai::IO::Net::Socket* socket, std::string& keyprint);

		/**
		 * The keyprint of the certificate we present to others, which is what
		 * belongs in the KP field of our own INF. False if we hold none - and
		 * then nothing should be published, because a keyprint for a
		 * certificate we do not have would fail every peer that checked it.
		 */
		static bool ofOwnCertificate(std::string& keyprint);

	private:
		static void format(const uint8_t* digest, std::string& keyprint);
};

}

#endif // HAVE_QUICKDC_KEYPRINT_H
