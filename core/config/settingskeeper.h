/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SETTINGSKEEPER_H
#define HAVE_QUICKDC_SETTINGSKEEPER_H

#include "config/preferences.h"

#include <samurai/timer.h>

#include <chrono>
#include <functional>
#include <memory>

namespace Samurai { class EventLoop; }

namespace QuickDC {

/**
 * Writes the settings stores to disk once they have been left alone for
 * 'quiet', and whatever is still unwritten when it is flushed or destroyed -
 * so a change is kept however it was made and whoever made it.
 */
class SettingsKeeper final : private PreferencesListener, public Samurai::TimerListener {
	public:
		SettingsKeeper(Samurai::EventLoop& loop, Preferences& config, Preferences& session,
		               std::chrono::milliseconds quiet = std::chrono::milliseconds(500));
		~SettingsKeeper() override;

		SettingsKeeper(const SettingsKeeper&) = delete;
		SettingsKeeper& operator=(const SettingsKeeper&) = delete;

		/** Write each store changed since it was last written, now. */
		void flush();

		/** Told of each write this makes, and whether it worked. */
		std::function<void(const Preferences& store, bool ok)> written;

	private:
		void EventSettingChanged(const Preferences& store, const std::string& group,
		                         const std::string& key, const std::string* value) override;
		void EventTimeout(Samurai::Timer* timer) override;
		void writeChanged();
		void report(const Preferences& store, bool ok);

		Samurai::EventLoop& loop;
		Preferences& config;
		Preferences& session;
		const std::chrono::milliseconds quiet;
		Subscription watchingConfig;
		Subscription watchingSession;
		std::unique_ptr<Samurai::Timer> soon;
		bool configChanged = false;
		bool sessionChanged = false;
};

}

#endif // HAVE_QUICKDC_SETTINGSKEEPER_H
