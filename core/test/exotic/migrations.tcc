/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Bringing an installation's settings forward, one version at a time.
 *
 * Each case starts from stores as an older client left them and checks both
 * what a step changed and what it was careful to leave alone.
 */

#include "quickdc.h"
#include "config/migrations.h"
#include "config/preferences.h"

#include <samurai/io/file.h>

#include <unistd.h>

#include <string>

namespace {

std::string scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_migrations_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/* Both stores over scratch files, removed again when this goes. */
struct Stores {
	std::string cfg = scratch("config.conf");
	std::string ses = scratch("session.conf");
	QuickDC::Preferences* config = nullptr;
	QuickDC::Preferences* session = nullptr;

	Stores() { open(); }

	void open()
	{
		delete config;
		delete session;
		config = new QuickDC::Preferences(cfg);
		session = new QuickDC::Preferences(ses);
	}

	~Stores()
	{
		delete config;
		delete session;
		Samurai::IO::File(cfg).remove();
		Samurai::IO::File(ses).remove();
	}
};

}

/* A first run gets the current version and the mode a new installation starts in. */
EXO_TEST(migrations_a_fresh_installation_starts_current_and_automatic,
{
	Stores s;
	QuickDC::migrateSettings(*s.config, *s.session, true);

	EXO_ASSERT_EQ_INT(QuickDC::settingsVersion(*s.config), QuickDC::currentSettingsVersion());
	EXO_ASSERT(s.config->peekString("Network", "Mode", "") == "automatic");
	return 1;
});

/* An installation from before modes keeps what it was doing, under a mode that says so. */
EXO_TEST(migrations_an_old_active_configuration_is_settled_as_advanced,
{
	Stores s;
	s.config->setBool("Network", "Active mode", true);

	QuickDC::migrateSettings(*s.config, *s.session, false);

	EXO_ASSERT(s.config->peekString("Network", "Mode", "") == "advanced");
	EXO_ASSERT_EQ_INT(QuickDC::settingsVersion(*s.config), QuickDC::currentSettingsVersion());
	return 1;
});

/*
 * What closing a session pinned is taken out; what somebody chose is kept.
 *
 * The global nickname copied into a hub, the product name a session fell back
 * to, and the global password copied the same way all go. A nickname that is
 * neither stays, and so does a group that is not a hub's.
 */
EXO_TEST(migrations_identities_pinned_by_closed_sessions_are_unpinned,
{
	Stores s;
	s.config->setString("User", "Nickname", "me");
	s.config->setString("User", "Password", "secret");

	s.session->setString("adc://a.example.org:1511", "Nickname", "me");
	s.session->setString("adc://a.example.org:1511", "Password", "secret");
	s.session->setString("adcs://b.example.org:1511", "Nickname", PRODUCT);
	s.session->setString("adcs://b.example.org:1511", "Password", "");
	s.session->setString("dchub://c.example.org:411", "Nickname", "chosen-here");
	s.session->setString("dchub://c.example.org:411", "Password", "its-own");
	s.session->setString("Windows", "Nickname", "me");

	QuickDC::migrateSettings(*s.config, *s.session, false);

	EXO_ASSERT(!s.session->haveSetting("adc://a.example.org:1511", "Nickname"));
	EXO_ASSERT(!s.session->haveSetting("adc://a.example.org:1511", "Password"));
	EXO_ASSERT(!s.session->haveSetting("adcs://b.example.org:1511", "Nickname"));
	EXO_ASSERT(!s.session->haveSetting("adcs://b.example.org:1511", "Password"));
	EXO_ASSERT(s.session->peekString("dchub://c.example.org:411", "Nickname", "") == "chosen-here");
	EXO_ASSERT(s.session->peekString("dchub://c.example.org:411", "Password", "") == "its-own");
	EXO_ASSERT(s.session->haveSetting("Windows", "Nickname"));
	return 1;
});

/* A step runs once: an installation already at the version is left exactly as it is. */
EXO_TEST(migrations_a_current_installation_is_left_alone,
{
	Stores s;
	s.config->setString("User", "Nickname", "me");
	QuickDC::migrateSettings(*s.config, *s.session, false);

	/* Pinned again by hand after the upgrade: somebody's choice now, not a leftover. */
	s.session->setString("adc://a.example.org:1511", "Nickname", "me");
	const uint64_t before = s.session->revision();

	EXO_ASSERT(!QuickDC::migrateSettings(*s.config, *s.session, false));
	EXO_ASSERT_EQ_UINT(s.session->revision(), before);
	EXO_ASSERT(s.session->haveSetting("adc://a.example.org:1511", "Nickname"));
	return 1;
});

/* What a migration wrote is on disk, so it does not depend on a later save. */
EXO_TEST(migrations_what_was_migrated_is_written_out,
{
	Stores s;
	s.config->setString("User", "Nickname", "me");
	s.session->setString("adc://a.example.org:1511", "Nickname", "me");
	s.config->write();
	s.session->write();

	QuickDC::migrateSettings(*s.config, *s.session, false);
	s.open();

	EXO_ASSERT_EQ_INT(QuickDC::settingsVersion(*s.config), QuickDC::currentSettingsVersion());
	EXO_ASSERT(!s.session->haveSetting("adc://a.example.org:1511", "Nickname"));
	return 1;
});
