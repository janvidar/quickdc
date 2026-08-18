/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "api/networkmode.h"
#include "config/preferences.h"

namespace {

const char GROUP[] = "Network";

const char MODE[] = "Mode";
const char ACTIVE[] = "Active mode";
const char USE_UPNP[] = "UseUPnP";
const char FORCE_ACTIVE[] = "Force active mode";
const char SERVER[] = "Reachability server";

const char AUTOMATIC[] = "automatic";
const char ADVANCED[] = "advanced";
const char PASSIVE[] = "passive";

}

const char* QuickDC::toString(QuickDC::NetworkMode mode)
{
	switch (mode)
	{
		case NetworkMode::Automatic: return AUTOMATIC;
		case NetworkMode::Advanced:  return ADVANCED;
		case NetworkMode::Passive:   return PASSIVE;
	}

	return ADVANCED;
}

QuickDC::NetworkMode QuickDC::networkModeFrom(const std::string& text,
                                              QuickDC::NetworkMode fallback)
{
	if (text == AUTOMATIC) return NetworkMode::Automatic;
	if (text == ADVANCED)  return NetworkMode::Advanced;
	if (text == PASSIVE)   return NetworkMode::Passive;

	return fallback;
}

const char* QuickDC::defaultReachabilityServer()
{
	return "drop.quickdc.org";
}

QuickDC::NetworkModeKeys QuickDC::networkModeKeys(QuickDC::NetworkMode mode,
                                                  bool storedUseUpnp,
                                                  bool storedForceActive,
                                                  const std::string& storedServer)
{
	NetworkModeKeys keys;

	/*
	 * Somebody has to be asked before a verdict can be shown, so an empty name
	 * becomes the default. Only where the mode asks for one: advanced answers with
	 * what the file holds, empty included, which is what lets an installation that
	 * predates the setting keep contacting nobody.
	 */
	keys.reachabilityServer = storedServer.empty() ? defaultReachabilityServer()
	                                               : storedServer;

	switch (mode)
	{
		case NetworkMode::Automatic:
			/*
			 * The intent is to be reachable; the gateway is asked in order to make
			 * it true, and the measurement withdraws the claim where it turns out
			 * not to be - see reachable() in network/adc/client/adchubsession.cpp.
			 * Forcing the claim is the one thing automatic must not do, since the
			 * whole of it is that the answer is established rather than asserted.
			 */
			keys.activeMode = true;
			keys.useUpnp = true;
			keys.forceActive = false;
			break;

		case NetworkMode::Advanced:
			keys.activeMode = true;
			keys.useUpnp = storedUseUpnp;
			keys.forceActive = storedForceActive;
			keys.reachabilityServer = storedServer;
			break;

		case NetworkMode::Passive:
			/*
			 * Nothing is told to connect here, so a forwarded port would be a hole
			 * in somebody's network that nothing is using - and a claim to be
			 * reachable would have peers dial a port this client is not offering.
			 */
			keys.activeMode = false;
			keys.useUpnp = false;
			keys.forceActive = false;
			break;
	}

	return keys;
}

QuickDC::NetworkMode QuickDC::storedNetworkMode(const QuickDC::Preferences& config)
{
	/*
	 * Advanced where the file says nothing, which is the reading that changes least:
	 * it answers with the values already there rather than imposing any.
	 */
	return networkModeFrom(config.peekString(GROUP, MODE, ""),
	                       NetworkMode::Advanced);
}

QuickDC::NetworkMode QuickDC::networkMode(QuickDC::Preferences& config,
                                          bool freshInstallation)
{
	if (config.haveSetting(GROUP, MODE))
		return storedNetworkMode(config);

	/*
	 * A file that was not there is somebody's first run, and gets the mode that
	 * needs no decisions. Everything else keeps what it had: the derived keys are
	 * not written, because in an existing file those keys are the state being
	 * described, and rewriting them is a chance to change behaviour for nothing.
	 */
	if (freshInstallation)
	{
		setNetworkMode(config, NetworkMode::Automatic);
		return NetworkMode::Automatic;
	}

	const NetworkMode settled = config.peekBool(GROUP, ACTIVE, false)
		? NetworkMode::Advanced
		: NetworkMode::Passive;

	config.setString(GROUP, MODE, toString(settled));

	/*
	 * Written now rather than left to the destructor, because a client that stops
	 * without one would decide this again at the next start - and the decision is
	 * made from values the first run may already have changed.
	 */
	config.write();

	return settled;
}

void QuickDC::setNetworkMode(QuickDC::Preferences& config, QuickDC::NetworkMode mode)
{
	const NetworkModeKeys keys = networkModeKeys(mode,
		config.peekBool(GROUP, USE_UPNP, false),
		config.peekBool(GROUP, FORCE_ACTIVE, false),
		config.peekString(GROUP, SERVER, ""));

	config.setString(GROUP, MODE, toString(mode));
	config.setBool(GROUP, ACTIVE, keys.activeMode);
	config.setBool(GROUP, USE_UPNP, keys.useUpnp);
	config.setBool(GROUP, FORCE_ACTIVE, keys.forceActive);
	config.setString(GROUP, SERVER, keys.reachabilityServer);
}
