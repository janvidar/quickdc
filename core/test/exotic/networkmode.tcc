/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * How much of getting connected the client works out for itself, and what that
 * implies for the plumbing under it.
 *
 * Two things are being pinned here and they pull in opposite directions. A new
 * installation should need no decisions: it measures against a server and asks the
 * gateway to forward a port, with nobody asked anything. An installation that
 * predates the setting must get neither of those without being asked, because both
 * were off by design and both tell somebody else that this machine is here. So the
 * migration below is mostly about what it declines to do.
 */

#include "quickdc.h"
#include "api/networkmode.h"
#include "config/preferences.h"

#include <samurai/io/file.h>

#include <stdio.h>
#include <string>
#include <samurai/posix.h>

using QuickDC::NetworkMode;
using QuickDC::NetworkModeKeys;

static std::string mode_scratch(const char* leaf)
{
	static int counter = 0;
	return "/tmp/quickdc_networkmode_" + std::to_string((int) getpid())
		+ "_" + std::to_string(counter++) + "_" + leaf;
}

/* A config file that removes itself when the case is over. */
class ModeConf
{
	public:
		explicit ModeConf(const char* leaf = "mode.conf")
			: path(mode_scratch(leaf)) { }

		~ModeConf() { Samurai::IO::File(path).remove(); }

		ModeConf(const ModeConf&) = delete;
		ModeConf& operator=(const ModeConf&) = delete;

		/** Put a file there, so the store reads as one somebody has been using. */
		bool write(const std::string& content) const
		{
			FILE* fh = fopen(path.c_str(), "wb");
			if (!fh) return false;
			const bool ok =
				fwrite(content.data(), 1, content.size(), fh) == content.size();
			fclose(fh);
			return ok;
		}

		std::string path;
};

/*
 * The mapping, which is a pure function so that the core and a front end cannot
 * come to different conclusions about the same file.
 */
EXO_TEST(networkmode_automatic_asks_for_everything_it_needs,
{
	const NetworkModeKeys keys =
		QuickDC::networkModeKeys(NetworkMode::Automatic, false, false, "");

	/* The intent to be reachable, and the gateway asked in order to make it so. */
	EXO_ASSERT(keys.activeMode);
	EXO_ASSERT(keys.useUpnp);

	/*
	 * And never the claim forced. The whole of automatic is that the answer is
	 * established rather than asserted, so a mode that asserted it would be
	 * announcing a reachability nothing had measured.
	 */
	EXO_ASSERT(!keys.forceActive);

	/* Somebody has to be asked, or there is no verdict to show. */
	EXO_ASSERT(keys.reachabilityServer == QuickDC::defaultReachabilityServer());
	return 1;
});

/*
 * Advanced answers with the file's own plumbing, because advanced means the port and
 * the forwarding are as somebody left them.
 */
EXO_TEST(networkmode_advanced_answers_with_what_the_file_holds,
{
	const NetworkModeKeys off =
		QuickDC::networkModeKeys(NetworkMode::Advanced, false, false, "");

	EXO_ASSERT(off.activeMode);
	EXO_ASSERT(!off.useUpnp);
	EXO_ASSERT(!off.forceActive);

	const NetworkModeKeys on =
		QuickDC::networkModeKeys(NetworkMode::Advanced, true, true, "drop.example.org");

	EXO_ASSERT(on.useUpnp);
	EXO_ASSERT(on.forceActive);

	/*
	 * A name the file already holds is never replaced. There is no field on the page
	 * for one, so this is how a server somebody runs themselves is configured, and
	 * overwriting it would take that away with no way to put it back.
	 */
	EXO_ASSERT(on.reachabilityServer == "drop.example.org");
	return 1;
});

/*
 * Every mode answers with somebody to ask, advanced included.
 *
 * With no field on the page for a server, a mode that answered with an empty name
 * would leave whoever chose it unable to measure and with nothing on screen saying
 * why. What keeps an upgraded installation from being opted in is the migration,
 * which settles the mode and touches nothing else - not this, which is only ever
 * reached because somebody chose a mode.
 */
EXO_TEST(networkmode_no_mode_is_left_with_nobody_to_ask,
{
	for (const NetworkMode mode : { NetworkMode::Automatic, NetworkMode::Advanced,
	                                NetworkMode::Passive })
	{
		const NetworkModeKeys keys = QuickDC::networkModeKeys(mode, false, false, "");

		EXO_ASSERT(keys.reachabilityServer == QuickDC::defaultReachabilityServer());
	}
	return 1;
});

/*
 * Passive forwards nothing and claims nothing. A forwarded port would be a hole
 * nothing is using, and a forced claim would have peers dial a port this client is
 * not offering.
 */
EXO_TEST(networkmode_passive_asks_for_nothing,
{
	const NetworkModeKeys keys =
		QuickDC::networkModeKeys(NetworkMode::Passive, true, true, "");

	EXO_ASSERT(!keys.activeMode);
	EXO_ASSERT(!keys.useUpnp);
	EXO_ASSERT(!keys.forceActive);
	return 1;
});

/* A mode named in the file survives the round trip; a typo does not become one. */
EXO_TEST(networkmode_an_unreadable_mode_is_not_taken_for_automatic,
{
	EXO_ASSERT(QuickDC::networkModeFrom("automatic", NetworkMode::Passive)
		== NetworkMode::Automatic);
	EXO_ASSERT(QuickDC::networkModeFrom("advanced", NetworkMode::Passive)
		== NetworkMode::Advanced);
	EXO_ASSERT(QuickDC::networkModeFrom("passive", NetworkMode::Automatic)
		== NetworkMode::Passive);

	/*
	 * Anything else is a file somebody edited by hand. Reading a typo as automatic
	 * would start measuring against a third party and searching the local segment
	 * on the strength of a misspelling.
	 */
	EXO_ASSERT(QuickDC::networkModeFrom("Automatic", NetworkMode::Advanced)
		== NetworkMode::Advanced);
	EXO_ASSERT(QuickDC::networkModeFrom("", NetworkMode::Advanced)
		== NetworkMode::Advanced);
	EXO_ASSERT(QuickDC::networkModeFrom("auto", NetworkMode::Passive)
		== NetworkMode::Passive);
	return 1;
});

/*
 * A first run gets the mode that needs no decisions, and the keys to go with it.
 */
EXO_TEST(networkmode_a_first_run_is_set_up_without_being_asked,
{
	ModeConf conf;

	QuickDC::Preferences prefs(conf.path);
	EXO_ASSERT(!prefs.existed());

	EXO_ASSERT(QuickDC::networkMode(prefs, !prefs.existed())
		== NetworkMode::Automatic);

	EXO_ASSERT(prefs.peekString("Network", "Mode", "") == "automatic");
	EXO_ASSERT(prefs.peekBool("Network", "Active mode", false));
	EXO_ASSERT(prefs.peekBool("Network", "UseUPnP", false));
	EXO_ASSERT(!prefs.peekBool("Network", "Force active mode", true));

	EXO_ASSERT(prefs.peekString("Network", "Reachability server", "")
		== QuickDC::defaultReachabilityServer());
	return 1;
});

/*
 * An installation that predates the setting is not moved to automatic, and this is
 * the case the whole migration exists for.
 *
 * Both of the things automatic does are disclosures somebody chose not to make:
 * measuring tells a third party this client's address and has it connect back, and
 * finding a gateway tells every host on the local segment what this machine runs.
 * Neither may begin because somebody upgraded. So it lands in advanced, which keeps
 * every value it had - and an empty server stays empty, which is what "contacts
 * nobody" is made of.
 */
EXO_TEST(networkmode_an_existing_installation_is_not_opted_in,
{
	ModeConf conf;

	EXO_ASSERT(conf.write(
		"[Network]\n"
		"Active mode=true\n"
		"Port=1411\n"
		"Reachability server=\n"
		"UseUPnP=false\n"));

	QuickDC::Preferences prefs(conf.path);
	EXO_ASSERT(prefs.existed());

	EXO_ASSERT(QuickDC::networkMode(prefs, !prefs.existed())
		== NetworkMode::Advanced);

	/* Nobody to ask, so nothing is asked. */
	EXO_ASSERT(prefs.peekString("Network", "Reachability server", "x").empty());

	/* And no gateway searched for. */
	EXO_ASSERT(!prefs.peekBool("Network", "UseUPnP", true));
	return 1;
});

/* A client that was passive stays passive, which is the one thing to read off it. */
EXO_TEST(networkmode_a_passive_installation_stays_passive,
{
	ModeConf conf;

	EXO_ASSERT(conf.write(
		"[Network]\n"
		"Active mode=false\n"
		"Port=1411\n"));

	QuickDC::Preferences prefs(conf.path);

	EXO_ASSERT(QuickDC::networkMode(prefs, !prefs.existed())
		== NetworkMode::Passive);
	EXO_ASSERT(prefs.peekString("Network", "Mode", "") == "passive");
	return 1;
});

/*
 * Settled once. Whatever the stored mode says, the migration does not run again -
 * which is what makes it idempotent with no version to keep in step, and what
 * stops a mode somebody chose from being overruled by the values under it.
 */
EXO_TEST(networkmode_a_settled_mode_is_never_decided_again,
{
	ModeConf conf;

	/* Passive named, and every key beneath it disagreeing. */
	EXO_ASSERT(conf.write(
		"[Network]\n"
		"Mode=passive\n"
		"Active mode=true\n"
		"UseUPnP=true\n"));

	QuickDC::Preferences prefs(conf.path);

	EXO_ASSERT(QuickDC::networkMode(prefs, !prefs.existed())
		== NetworkMode::Passive);

	/* Read, not rewritten: the derived keys are left exactly as they were. */
	EXO_ASSERT(prefs.peekBool("Network", "Active mode", false));
	EXO_ASSERT(prefs.peekBool("Network", "UseUPnP", false));
	return 1;
});

/* Choosing a mode writes it and everything it decides. */
EXO_TEST(networkmode_choosing_a_mode_writes_what_it_decides,
{
	ModeConf conf;

	EXO_ASSERT(conf.write(
		"[Network]\n"
		"Mode=advanced\n"
		"Active mode=true\n"
		"UseUPnP=true\n"
		"Force active mode=true\n"));

	QuickDC::Preferences prefs(conf.path);

	QuickDC::setNetworkMode(prefs, NetworkMode::Passive);

	EXO_ASSERT(prefs.peekString("Network", "Mode", "") == "passive");
	EXO_ASSERT(!prefs.peekBool("Network", "Active mode", true));
	EXO_ASSERT(!prefs.peekBool("Network", "UseUPnP", true));
	EXO_ASSERT(!prefs.peekBool("Network", "Force active mode", true));

	/* And a server to ask, since passive still shows the address it is seen at. */
	EXO_ASSERT(!prefs.peekString("Network", "Reachability server", "").empty());
	return 1;
});

/*
 * Asking what mode a store records changes nothing.
 *
 * It is read on paths that only want to know - a front end drawing the page, a
 * hub session building an INF - and the ordinary accessors write their default back
 * when a key is missing. One of those on this path would create the key, which is
 * the signal the migration reads.
 */
EXO_TEST(networkmode_reading_the_mode_does_not_settle_it,
{
	ModeConf conf;

	EXO_ASSERT(conf.write("[Network]\nActive mode=true\n"));

	QuickDC::Preferences prefs(conf.path);

	EXO_ASSERT(QuickDC::storedNetworkMode(prefs) == NetworkMode::Advanced);
	EXO_ASSERT(!prefs.haveSetting("Network", "Mode"));
	return 1;
});

/* The mode decides whether this client is active; the stored key does not. */
EXO_TEST(networkmode_passive_is_passive_whatever_active_mode_says,
{
	ModeConf conf;

	EXO_ASSERT(conf.write(
		"[Network]\n"
		"Mode=passive\n"
		"Active mode=true\n"
		"Force active mode=true\n"));

	QuickDC::Preferences prefs(conf.path);

	const NetworkModeKeys keys = QuickDC::networkModeKeysInForce(prefs);
	EXO_ASSERT(!keys.activeMode);
	EXO_ASSERT(!keys.forceActive);
	return 1;
});

/* A store with no mode is read the way the migration would settle it. */
EXO_TEST(networkmode_a_store_with_no_mode_is_read_by_its_active_key,
{
	ModeConf on("on.conf");
	ModeConf off("off.conf");

	EXO_ASSERT(on.write("[Network]\nActive mode=true\n"));
	EXO_ASSERT(off.write("[Network]\nActive mode=false\n"));

	QuickDC::Preferences active(on.path);
	QuickDC::Preferences passive(off.path);

	EXO_ASSERT(QuickDC::networkModeInForce(active) == NetworkMode::Advanced);
	EXO_ASSERT(QuickDC::networkModeInForce(passive) == NetworkMode::Passive);
	EXO_ASSERT(QuickDC::networkModeKeysInForce(active).activeMode);
	EXO_ASSERT(!QuickDC::networkModeKeysInForce(passive).activeMode);
	EXO_ASSERT(!active.haveSetting("Network", "Mode"));
	return 1;
});

/* Advanced takes the user's choices; the other modes decide them. */
EXO_TEST(networkmode_advanced_takes_the_choices_it_is_given,
{
	ModeConf conf;
	EXO_ASSERT(conf.write("[Network]\nMode=automatic\nUseUPnP=true\n"));

	QuickDC::Preferences prefs(conf.path);

	QuickDC::setNetworkMode(prefs, NetworkMode::Advanced, false, true);
	EXO_ASSERT(prefs.peekString("Network", "Mode", "") == "advanced");
	EXO_ASSERT(!prefs.peekBool("Network", "UseUPnP", true));
	EXO_ASSERT(prefs.peekBool("Network", "Force active mode", false));

	QuickDC::setNetworkMode(prefs, NetworkMode::Automatic, false, true);
	EXO_ASSERT(prefs.peekBool("Network", "UseUPnP", false));
	EXO_ASSERT(!prefs.peekBool("Network", "Force active mode", true));
	return 1;
});
