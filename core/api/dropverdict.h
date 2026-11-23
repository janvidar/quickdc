/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DROPVERDICT_H
#define HAVE_QUICKDC_DROPVERDICT_H

#include <cstdint>
#include <string>
#include <time.h>
#include <vector>

#include "api/subscription.h"

/*
 * What a reachability measurement concluded, and nothing about how.
 *
 * Deliberately free of the measuring library's own types. The verdict crosses
 * out of the core to whatever is showing it, and a header that named Drop's
 * enums would put Drop's headers on the include path of everything that reads
 * one - including a front end that links this library privately precisely so
 * that it cannot reach the core's own internals either. The words are already
 * strings by the time they get here, which is what they are wanted as.
 */

namespace QuickDC {

class Core;

/** What the measurement says this client should do. */
enum class DropMode
{
	/** Nothing was established. Change no setting on the strength of this. */
	Unknown,

	/** Something outside reached the advertised port. */
	Active,

	/** Nothing did. */
	Passive
};

const char* toString(DropMode mode);

/**
 * Whether the measurement got through to one port.
 *
 * Beside the words below rather than derived from them, because the two are for
 * different readers: the strings are what a person is shown, and this is what a
 * decision is made on. Reducing one to the other would mean matching on display
 * text, which changes when the wording does.
 *
 * 'Unknown' is not a soft 'Blocked'. A probe the server could not perform says
 * nothing whatever about this client, and treating that as blocked would withdraw
 * an active claim on the strength of somebody else's outage.
 */
enum class DropReach { Unknown, Reached, Blocked };

struct DropVerdict
{
	/** False until a check has completed; every field below is then unset. */
	bool ran = false;

	/** Which family was measured. One run answers for one of them. */
	bool ipv6 = false;

	/** The address the server saw, which need not be one of this host's. */
	std::string publicAddress;

	/** How the server classified that address, in its own words. */
	std::string classification;

	DropMode mode = DropMode::Unknown;

	/* The per-port outcomes, named. Empty where the test was not run. */
	std::string tcp;
	std::string udp;

	/* And the same two as something to decide on; see DropReach. */
	DropReach tcpReached = DropReach::Unknown;
	DropReach udpReached = DropReach::Unknown;

	/** The port that was tested, which is the one this client advertises. */
	uint16_t port = 0;

	/*
	 * Written for a user by whoever measured, and to be shown as they are.
	 *
	 * Not rephrased on the way past: a user who moves between two clients and is
	 * given two different explanations of the same network concludes that one of
	 * them is broken.
	 */
	std::string reason;
	std::vector<std::string> remedies;
	std::string note;

	/** When it was established, for deciding whether to ask again. */
	time_t observedAt = 0;

	/**
	 * True where nothing connected back, so this is an address and not a measurement.
	 *
	 * A lookup asks a DROP server only what address it sees and stops there: it is
	 * what a client that has declared itself passive can still be shown, since no
	 * stranger dials its peer port for it. Everything about reachability is then
	 * unset - the mode is unknown and both reach values are Unknown - and that is
	 * not the same as having measured and found nothing.
	 *
	 * Marked rather than inferred from the empty fields, because the two callers
	 * that must not confuse the one for the other decide on this: a verdict from a
	 * lookup is never fresh evidence, and never written into the reachability
	 * record, or looking an address up would suppress the next real measurement for
	 * a quarter of an hour and overwrite what the last one established.
	 */
	bool addressOnly = false;
};

class DropCheckListener
{
	public:
		virtual ~DropCheckListener() { }

		/**
		 * A run has begun, and one of the two below will follow it.
		 *
		 * Not pure, unlike the two that report an outcome: a listener that only
		 * records what was established has nothing to do here, and every one of
		 * them would otherwise carry an empty override. What it is for is a front
		 * end saying so on screen - checks are started by the core as well as by a
		 * user, and a page that only knew about the ones it asked for itself would
		 * sit still through the rest.
		 *
		 * @param ipv6 which family the run is about.
		 */
		virtual void EventDropCheckStarted(bool ipv6) { (void) ipv6; }

		/** A run completed, whatever it concluded. */
		virtual void EventDropCheckFinished(const DropVerdict& verdict) = 0;

		/**
		 * A run could not be completed, and nothing was established.
		 *
		 * 'reason' is a sentence fit to show whoever is looking at the
		 * connection settings, and is never null. A failure here says nothing
		 * about whether this client is reachable, only that nobody measured it.
		 */
		virtual void EventDropCheckFailed(const char* reason) = 0;
};

/*
 * The three things anything outside the core wants to do with the measurement.
 *
 * Free functions rather than methods, and declared here rather than on the check
 * itself, because calling a method means having the class definition and that one
 * names the measuring library's types. A front end links this core privately
 * precisely so that it cannot reach those, so the operations are offered where the
 * verdict is: no include of Drop, and nothing to keep in step but these.
 *
 * All three must be called on the core's own thread, like everything else that
 * touches it. They do nothing at all when no server is configured, which is the
 * default and the case a caller should expect.
 */

/**
 * Also report every verdict to 'listener', or to nobody when null.
 *
 * Beside what the core does with one, never instead of it. The core records the
 * measurement, asks the gateway to forward a port that nothing reached, and has
 * the hubs republish; none of that is a front end's to take over by drawing the
 * same answer, and a client that behaved differently depending on whether a
 * window was open would be two different clients.
 *
 * A run in flight is left going. It was started for the core's own reasons and
 * reports to the core either way, so there is nothing to abandon - and the run
 * this displaces at start-up would be the one the core began on launch.
 *
 * Does nothing in a core built without a listening socket, where there is no
 * port worth measuring and so no check to attach to.
 */
void setDropCheckListener(Core& core, DropCheckListener* listener);

/** Also report to 'listener' until the subscription ends; null without a check. */
Subscription observeDropCheck(Core& core, DropCheckListener* listener);

/**
 * Measure again, reading the server and the port from the configuration as it
 * stands now rather than as it stood at start-up.
 *
 * The family's own hostname is what is asked, never the dual-stack name: one
 * connection has one source address, so the unprefixed name would measure
 * whichever family that connection used rather than the one being asked about.
 *
 * @param ipv6 which family to measure. One run answers for one of them, because
 *        one run reaches the client over one of them - and the two answers
 *        routinely differ, native IPv6 alongside an IPv4 address behind a
 *        carrier-grade NAT being the ordinary home connection.
 *
 * @return false when there is nothing to ask, which is not a failure: no server
 *         is configured, or a run is already going and will report by itself.
 *         Asking a second time would only spend a session the server counts
 *         against us.
 */
bool startDropCheck(Core& core, bool ipv6);

/**
 * Ask a DROP server only what address it sees this client at, and stop there.
 *
 * The same server and the same family-specific hostname as startDropCheck(), and
 * everything else about it left undone: no session is opened, nothing is armed to
 * answer a stranger, and no port is probed - so this is what a client which is not
 * trying to be reachable can still be shown, and it needs no peer port to ask.
 *
 * The verdict it leaves carries the public address and how the server classified
 * it, with 'addressOnly' set. That mark is what keeps it out of the reachability
 * record and out of the freshness test, so a lookup can be taken as often as a
 * page is opened without displacing a measurement.
 *
 * @param ipv6 which family to ask about. One run answers for one of them, since
 *        one connection carries one source address.
 *
 * @return false when there is nothing to ask: no server is configured, or a run is
 *         already going - a lookup shares the one client with a measurement, and
 *         the measurement is the one worth having.
 */
bool startAddressLookup(Core& core, bool ipv6);

/**
 * The last verdict for 'ipv6', with ran false where that family has not been
 * measured - which is not the same as having been measured and found blocked.
 */
DropVerdict lastDropVerdict(const Core& core, bool ipv6);

/**
 * Whether 'held' still describes the network, as of 'now'.
 *
 * The same age at which the reachability record stops weighing a measurement, so
 * that what is believed and what is worth measuring again cannot disagree - see
 * QUICKDC_REACHABILITY_MEASUREMENT_AGE for why a verdict is believed for a while
 * and then ignored entirely.
 *
 * A verdict that never ran is not fresh, and neither is one dated in the future:
 * these are wall-clock times, so an operator correcting a clock or a machine
 * resuming with a bad one can leave a measurement apparently taken later than the
 * moment it is being judged from. Treating that as fresh would hold a stale
 * answer for however far the clock was out.
 *
 * Neither is an address-only verdict, however recent: nothing connected back for
 * it, so there is no measurement in it to be fresh, and answering yes would let a
 * lookup taken when a settings page opened stand in for the real run this is asked
 * in order to schedule.
 *
 * Unlike the three above this touches nothing and may be called from anywhere.
 */
bool dropMeasurementIsFresh(const DropVerdict& held, time_t now);

/**
 * Feed the last verdict into the reachability record again.
 *
 * Needed because the record ignores a measurement until something has been
 * advertised - a verdict is about an endpoint, and before a login there is none
 * for it to be about. The check at start-up routinely finishes first, so the
 * measurement it took has to be offered again once there is an endpoint. Harmless
 * where there is nothing to offer, or where it has already been applied.
 *
 * An address-only verdict is passed over. Its reach values are Unknown because
 * nothing was probed, and writing those into the record would take away what a
 * real measurement had established.
 */
void applyDropMeasurement(Core& core);

}

#endif // HAVE_QUICKDC_DROPVERDICT_H
