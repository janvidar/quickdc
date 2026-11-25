/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "config/migrations.h"

#include "api/networkmode.h"
#include "config/preferences.h"
#include "config/settings.h"
#include "quickdc.h"

#include <string>

namespace {

const char GROUP[] = "Settings";
const char VERSION_KEY[] = "Version";

/*
 * Version 1: a connection mode for a configuration that predates them, worked
 * out from what it already said rather than moving it to the mode that measures
 * and searches the local segment. A fresh installation starts in automatic.
 */
void settleConnectionMode(QuickDC::Preferences& config, QuickDC::Preferences&, bool fresh)
{
	QuickDC::networkMode(config, fresh);
}

/*
 * Version 2: what closing a hub session used to pin as that hub's own identity.
 *
 * A session wrote the nickname and password it had used back into the hub's
 * group as it closed, so whatever was global at the time became the hub's own
 * for good and outranked every later change to the setting. Such an entry
 * cannot be told for certain from one somebody chose; what is removed is what
 * says nothing the global setting does not - the same nickname or password as
 * the global one, or the product name a session fell back to when there was
 * none. Removing those changes nothing about who this client is on any hub
 * today, and lets a later change to the setting reach them.
 */
void unpinHubIdentities(QuickDC::Preferences& config, QuickDC::Preferences& session, bool)
{
	namespace S = QuickDC::Settings;

	const std::string nickname = S::read(config, S::Nickname);
	const std::string password = S::read(config, S::Password);

	for (const std::string& group : session.groupNames())
	{
		if (group.find("://") == std::string::npos) continue;

		const std::string pinned = session.peekString(group, "Nickname", "");
		if (session.haveSetting(group, "Nickname")
		    && (pinned.empty() || pinned == nickname || pinned == PRODUCT))
			session.remove(group, "Nickname");

		const std::string kept = session.peekString(group, "Password", "");
		if (session.haveSetting(group, "Password") && (kept.empty() || kept == password))
			session.remove(group, "Password");
	}
}

/*
 * Version 3: the reachability server, which the settings page stopped offering a
 * choice of once every mode measured against the same one. A server stored before
 * that still outranked the default, and with nothing on the page to change it a
 * server that had gone away left every check failing for good. A stored one other
 * than the default is put back to it; an empty one is left empty, since nothing
 * stored is how the client is kept from asking a third party at all. A first run
 * has nothing from before the page changed, so a server given with it is a choice.
 */
void resetReachabilityServer(QuickDC::Preferences& config, QuickDC::Preferences&, bool freshInstallation)
{
	namespace S = QuickDC::Settings;

	if (freshInstallation) return;

	const std::string stored = S::read(config, S::ReachabilityServer);
	if (stored.empty() || stored == QuickDC::defaultReachabilityServer()) return;

	S::write(config, S::ReachabilityServer, QuickDC::defaultReachabilityServer());
}

using Step = void (*)(QuickDC::Preferences&, QuickDC::Preferences&, bool);

/* In order; a step's version is its position plus one. Only ever appended to. */
const Step STEPS[] = {
	settleConnectionMode,
	unpinHubIdentities,
	resetReachabilityServer,
};

}

int QuickDC::currentSettingsVersion()
{
	return static_cast<int>(sizeof(STEPS) / sizeof(STEPS[0]));
}

int QuickDC::settingsVersion(const Preferences& config)
{
	return config.peekNumber(GROUP, VERSION_KEY, 0);
}

bool QuickDC::migrateSettings(Preferences& config, Preferences& session, bool freshInstallation)
{
	const int from = settingsVersion(config);
	const int to = currentSettingsVersion();
	if (from >= to) return false;

	const uint64_t before = config.revision() + session.revision();

	for (int version = from; version < to; ++version)
		STEPS[version](config, session, freshInstallation);

	config.setNumber(GROUP, VERSION_KEY, to);

	config.write();
	session.write();
	return config.revision() + session.revision() != before;
}
