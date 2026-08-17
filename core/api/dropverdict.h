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
};

class DropCheckListener
{
	public:
		virtual ~DropCheckListener() { }

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
 * Point the measurement at 'listener', replacing whatever the core set up.
 *
 * A measurement is made on demand where the core has none, so that a server
 * named after this started is usable without a restart: the check is only absent
 * because nothing had been configured to ask.
 *
 * A run in flight belongs to whoever is being displaced, and is abandoned rather
 * than left going. The connect-back is recognised through whichever check this
 * names - the protocol probe asks it - so a run whose listener has changed would
 * answer the challenge and report to somebody who is no longer there, and one
 * whose listener is gone would conclude passive on no evidence.
 */
void setDropCheckListener(DropCheckListener* listener);

/**
 * Measure again, reading the server and the port from the configuration as it
 * stands now rather than as it stood at start-up.
 *
 * The family's own hostname is what is asked, never the dual-stack name: one
 * connection has one source address, so the unprefixed name would measure
 * whichever family that connection used rather than the one being asked about.
 *
 * @return false when there is nothing to ask, which is not a failure: no server
 *         is configured, or a run is already going and will report by itself.
 *         Asking a second time would only spend a session the server counts
 *         against us.
 */
bool startDropCheck();

/** The last verdict, with ran false when nothing has been measured. */
DropVerdict lastDropVerdict();

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
 */
void applyDropMeasurement();

}

#endif // HAVE_QUICKDC_DROPVERDICT_H
