/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "config/settingskeeper.h"

#include <samurai/eventloop.h>

QuickDC::SettingsKeeper::SettingsKeeper(Samurai::EventLoop& loop_, Preferences& config_,
                                        Preferences& session_, std::chrono::milliseconds quiet_)
	: loop(loop_)
	, config(config_)
	, session(session_)
	, quiet(quiet_)
	, watchingConfig(config_.subscribe(this))
	, watchingSession(session_.subscribe(this))
{
}

QuickDC::SettingsKeeper::~SettingsKeeper()
{
	flush();
}

void QuickDC::SettingsKeeper::flush()
{
	soon.reset();
	writeChanged();
}

void QuickDC::SettingsKeeper::writeChanged()
{
	const bool writeConfig = configChanged;
	const bool writeSession = sessionChanged;
	configChanged = false;
	sessionChanged = false;

	if (writeConfig) report(config, config.write());
	if (writeSession) report(session, session.write());
}

void QuickDC::SettingsKeeper::EventSettingChanged(const Preferences& store, const std::string&,
                                                  const std::string&, const std::string*)
{
	if (&store == &config) configChanged = true;
	else sessionChanged = true;

	/* Restarted, so a burst of changes is one write after it. */
	soon = std::make_unique<Samurai::Timer>(loop, this, quiet, true);
}

void QuickDC::SettingsKeeper::report(const Preferences& store, bool ok)
{
	if (written) written(store, ok);
}

/* Not flush(): the timer is still running this call and is not ours to free here. */
void QuickDC::SettingsKeeper::EventTimeout(Samurai::Timer*)
{
	writeChanged();
}
