/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_NETWORKMODE_H
#define HAVE_QUICKDC_NETWORKMODE_H

#include <string>

/*
 * How much of getting connected the client is asked to work out for itself.
 *
 * The settings under [Network] are the plumbing - a port, an address to bind, a
 * gateway to ask, a server to measure against - and every one of them is a
 * question somebody has to understand NAT to answer. This is the one setting that
 * is not: it says how much of the rest to decide without asking, and the rest is
 * derived from it.
 *
 * Deliberately free of the measuring library's types, like api/dropverdict.h
 * beside it, so that a front end can include this and reach nothing else.
 */

namespace QuickDC {

class Preferences;

enum class NetworkMode
{
	/**
	 * Pick the port, ask the gateway to forward it, and measure the result.
	 *
	 * What a client should do unless somebody says otherwise, and what a new
	 * installation gets. It measures against a third party and searches the local
	 * segment for a gateway, neither of which a client does unbidden - so an
	 * installation that predates this setting is never moved here by the
	 * migration; see networkMode().
	 */
	Automatic,

	/** The plumbing is the user's to set, and they are shown whether it worked. */
	Advanced,

	/** Nothing is forwarded and nothing connects in. */
	Passive
};

/** The spelling stored in [Network] Mode. */
const char* toString(NetworkMode mode);

/**
 * The mode 'text' names, or 'fallback' where it names none.
 *
 * Never guesses. An unrecognised word is a file somebody edited by hand, and
 * reading a typo as automatic would start measuring against a third party and
 * announcing this machine to the local segment on the strength of it.
 */
NetworkMode networkModeFrom(const std::string& text, NetworkMode fallback);

/** The server a client measures against unless its file names another. */
const char* defaultReachabilityServer();

/**
 * What a mode implies for the settings the rest of the core reads.
 *
 * The whole of the mapping, in one place and as a function of its arguments. Both
 * the core and a front end need the answer and neither can ask the other for it -
 * the core settles it before a bridge exists, and a settings page must not wait on
 * the core thread - so what they share is this, which needs nothing from either.
 *
 * 'storedUseUpnp', 'storedForceActive' and 'storedServer' are what the file holds
 * now. Advanced answers with them, because advanced means the plumbing is as
 * somebody left it. The other two answer with what the mode requires, except that
 * an empty stored server becomes the default rather than staying empty: a mode that
 * shows a verdict needs somebody to ask.
 */
struct NetworkModeKeys
{
	bool activeMode = true;
	bool useUpnp = false;
	bool forceActive = false;
	std::string reachabilityServer;
};

NetworkModeKeys networkModeKeys(NetworkMode mode, bool storedUseUpnp,
                                bool storedForceActive,
                                const std::string& storedServer);

/**
 * The mode in force, settling it once for a store that has none.
 *
 * Called before anything else reads [Network], and answers without changing the
 * store except on the single occasion when the key is absent. Reads go through the
 * peeking accessors, because the ordinary ones write their default back and a
 * question asked here would then have answered itself.
 *
 * What a store with no Mode is given:
 *
 *   - no file at all           automatic, and the derived keys written with it
 *   - Active mode false        passive
 *   - anything else            advanced
 *
 * Only the first writes more than the key. An installation that predates this
 * setting keeps every value it had, because those values *are* the state being
 * described - and it lands in advanced, where an empty reachability server stays
 * empty and nothing is contacted. Moving it to automatic would be the client
 * opting somebody into measuring and searching because they upgraded.
 *
 * @param freshInstallation whether there was no file; see Preferences::existed().
 */
NetworkMode networkMode(Preferences& config, bool freshInstallation);

/** The mode a store already records, without settling or writing anything. */
NetworkMode storedNetworkMode(const Preferences& config);

/** Write 'mode' and every setting it derives. */
void setNetworkMode(Preferences& config, NetworkMode mode);

}

#endif // HAVE_QUICKDC_NETWORKMODE_H
