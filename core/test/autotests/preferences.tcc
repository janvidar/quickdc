#include <samurai/samurai.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "config/preferences.h"

/*
 * Session data for every hub lives in one file, and a Preferences holds all of
 * it in memory and writes all of it back. That is why QuickDC::Core owns a
 * single session store that every hub session borrows: a store per session
 * wrote a snapshot taken before the other sessions existed, silently dropping
 * their settings - a remembered keyprint among them, which turns a substituted
 * hub certificate back into an unremarkable first connection.
 *
 * These cover the invariants that arrangement depends on.
 */

static std::string prefs_test_path()
{
	return std::string("/tmp/quickdc_prefs_test.conf");
}

static void prefs_test_unlink()
{
	remove(prefs_test_path().c_str());
}

/* Two groups written through one store must both survive a round trip. */
EXO_TEST(prefs_two_groups_round_trip, {
	prefs_test_unlink();

	{
		QuickDC::Preferences p(prefs_test_path());
		p.setGroup("adc://hub-a:411");
		p.setString("Keyprint", "SHA256/AAAA");
		p.setGroup("adc://hub-b:412");
		p.setString("Keyprint", "SHA256/BBBB");
		p.write();
	}

	QuickDC::Preferences again(prefs_test_path());
	again.setGroup("adc://hub-a:411");
	bool a_ok = again.haveSetting("Keyprint")
		&& strcmp(again.getString("Keyprint", ""), "SHA256/AAAA") == 0;

	again.setGroup("adc://hub-b:412");
	bool b_ok = again.haveSetting("Keyprint")
		&& strcmp(again.getString("Keyprint", ""), "SHA256/BBBB") == 0;

	prefs_test_unlink();
	return a_ok && b_ok;
});

/*
 * The current group is shared state, so a value must land in whichever group was
 * selected last - never in one selected earlier by somebody else.
 */
EXO_TEST(prefs_group_selection_is_not_sticky, {
	prefs_test_unlink();

	{
		QuickDC::Preferences p(prefs_test_path());
		p.setGroup("adc://hub-a:411");
		p.setString("Nickname", "mine");

		/* Another session moves the group, then the first one writes again. */
		p.setGroup("adc://hub-b:412");
		p.setGroup("adc://hub-a:411");
		p.setString("Password", "secret");
		p.write();
	}

	QuickDC::Preferences again(prefs_test_path());
	again.setGroup("adc://hub-a:411");
	bool mine_ok = strcmp(again.getString("Nickname", ""), "mine") == 0
		&& strcmp(again.getString("Password", ""), "secret") == 0;

	again.setGroup("adc://hub-b:412");
	bool other_clean = !again.haveSetting("Password");

	prefs_test_unlink();
	return mine_ok && other_clean;
});

/* A keyprint filed under the canonical URL must not collide with the typed one. */
EXO_TEST(prefs_keyprint_group_distinct_from_address, {
	prefs_test_unlink();

	{
		QuickDC::Preferences p(prefs_test_path());
		p.setGroup("adc://hub:411?kp=SHA256/ZZZZ");
		p.setString("Password", "typed");
		p.setGroup("adc://hub:411");
		p.setString("Keyprint", "SHA256/ZZZZ");
		p.write();
	}

	QuickDC::Preferences again(prefs_test_path());
	again.setGroup("adc://hub:411");
	bool kp_ok = strcmp(again.getString("Keyprint", ""), "SHA256/ZZZZ") == 0;

	again.setGroup("adc://hub:411?kp=SHA256/ZZZZ");
	bool pw_ok = strcmp(again.getString("Password", ""), "typed") == 0;

	prefs_test_unlink();
	return kp_ok && pw_ok;
});
