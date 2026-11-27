/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_UPDATE_VERSION_H
#define HAVE_QUICKDC_UPDATE_VERSION_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace QuickDC {
namespace Update {

/**
 * A version as packages are stamped (packaging/version.sh): X.Y.Z for a release,
 * X.Y.Z~nightlyYYYYMMDD.<commit> for a nightly, X.Y.Z+<describe> for anything
 * built by hand.
 */
struct Version {
	unsigned major = 0;
	unsigned minor = 0;
	unsigned patch = 0;
	/** A nightly, and the day it was built. */
	bool nightly = false;
	uint32_t date = 0;
	std::string commit;
	/** Built by hand: no channel offers it, so it is never replaced. */
	bool local = false;
};

std::optional<Version> parseVersion(std::string_view text);

/**
 * Whether 'offered' comes after 'running', as Debian orders them: a nightly is
 * before the release of its number, nightlies follow their dates, and two of one
 * day are the same.
 */
bool isNewer(const Version& offered, const Version& running);

}
}

#endif // HAVE_QUICKDC_UPDATE_VERSION_H
