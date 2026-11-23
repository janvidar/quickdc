/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONFIG_MIGRATIONS_H
#define HAVE_QUICKDC_CONFIG_MIGRATIONS_H

namespace QuickDC {

class Preferences;

/** The version settingsVersion() reaches once every step below has run. */
int currentSettingsVersion();

/** What [Settings] Version says; 0 for a configuration that predates versions. */
int settingsVersion(const Preferences& config);

/**
 * Bring both stores up to the current version, one step at a time, and record
 * where they got to. Each step runs once in the life of an installation.
 *
 * @param freshInstallation the configuration file did not exist: there is
 *        nothing to bring forward, only defaults to choose.
 * @return whether anything was written.
 */
bool migrateSettings(Preferences& config, Preferences& session, bool freshInstallation);

}

#endif // HAVE_QUICKDC_CONFIG_MIGRATIONS_H
