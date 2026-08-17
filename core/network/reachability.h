/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_REACHABILITY_H
#define HAVE_QUICKDC_REACHABILITY_H

#include "quickdc.h"

#include <set>
#include <string>
#include <time.h>

/*
 * How many peers have to report a failed connection before the claim is dropped.
 *
 * More than one, because a single failure is as likely to be the other end's
 * problem as ours - a firewall of their own, a client that got the port wrong, or
 * a peer that would rather we stopped advertising. Distinct peers, so one of them
 * cannot reach the threshold by repeating itself.
 */
#define QUICKDC_REACHABILITY_REFUSALS 3

/*
 * And how many are remembered while getting there. Reports past the threshold
 * change nothing, so there is no reason to keep growing the set.
 */
#define QUICKDC_REACHABILITY_MAX_REPORTERS 16

/*
 * How many seconds a measurement is taken to still describe the network.
 *
 * A verdict measured from outside is about the network the client was on when it
 * was taken, and a laptop that has moved since holds one about a network it is no
 * longer on. Nothing announces that move here - the endpoint can be unchanged, or
 * changed and announced again minutes later - so the measurement is believed for a
 * while and then ignored entirely, as though it had never been taken. Fifteen
 * minutes is long enough to answer for a session's worth of decisions and short
 * enough that a verdict does not outlive the network it describes.
 */
#define QUICKDC_REACHABILITY_MEASUREMENT_AGE 900

namespace QuickDC {

/**
 * What is known about being reachable from outside, for one advertised endpoint.
 *
 * Nothing here is asked of the network: reachability cannot be established except
 * by traffic arriving unsolicited from outside, and this client already sends what
 * would provoke it. A connect request is a prediction that a connection will
 * arrive; a search is a prediction that a datagram will. Both predictions are
 * already made and the outcomes already reported - and were, until now, thrown
 * away.
 *
 * A measurement changes none of that: something else arranges for a server outside
 * to connect back, and what reaches here is only what it concluded. It is weighed
 * as what it is - an observation made from outside, which outranks anything
 * inferred from what other people report about their own attempts, and is itself
 * outranked by traffic that actually arrived. See tcp() for the whole order, which
 * is the only real question this class answers.
 *
 * Evidence belongs to the endpoint it was gathered for. Advertise a different
 * address or port and everything learned about the old one says nothing about the
 * new one, so it is discarded - see advertising().
 *
 * Not detected: a mapping that dies while the endpoint stays the same, which is
 * what a lapsed UPnP lease looks like. Evidence that arrived is not aged out here,
 * so that case has to be caught by asking the gateway whether the mapping is
 * still there rather than by waiting for this to change its mind. A measurement is
 * aged out, for a different reason - see QUICKDC_REACHABILITY_MEASUREMENT_AGE.
 */
class Reachability
{
	public:
		enum class State
		{
			Unknown,      /**< Nothing has been observed either way. */
			Reachable,    /**< Something arrived from outside. */
			Unreachable   /**< Enough peers said they could not get in. */
		};

		/** A name for a state, for a diagnostic. */
		static const char* toString(State state);

		/**
		 * The endpoint being advertised from now on.
		 *
		 * Discards everything gathered for a different one. Safe to call on every
		 * announcement: the same endpoint twice changes nothing.
		 */
		void advertising(const std::string& address, uint16_t port);

		/**
		 * A connection arrived on the peer port from somewhere that is not this
		 * machine.
		 *
		 * Proof, and the reason a caller must not pass a loopback connection: the
		 * loopback proves only that the socket is open, which was never in doubt.
		 */
		void inboundConnection();

		/**
		 * A datagram arrived that answers something we sent.
		 *
		 * Answering matters. A source address on a datagram is whatever the sender
		 * wrote, so an unsolicited one proves nothing; one carrying a token this
		 * client issued could only come from somewhere that was told the token.
		 */
		void inboundDatagram();

		/**
		 * A peer we asked to connect reported that it could not.
		 *
		 * @param peer who said so, so that one peer repeating itself counts once.
		 *        Callers must only pass this on for a token this client issued -
		 *        otherwise anyone on the hub could argue us into being passive.
		 */
		void connectionRefused(const std::string& peer);

		/**
		 * A hub we are logged in to tried the port we published and got nothing.
		 *
		 * Decisive on its own, where a peer's report is not, because it is not the
		 * same kind of statement. A peer's DSTA is hearsay: it arrives through the
		 * hub, quoting a token anyone on the hub saw, and describes a failure only
		 * that peer witnessed. This is a party that made the attempt itself and
		 * says so over the connection we authenticated to it.
		 *
		 * Trusting it grants an operator nothing new either - a hub that wanted its
		 * users passive could simply decline to relay their connect requests.
		 *
		 * Still outranked by a connection that actually arrived: the check may have
		 * run before a port mapping came up, and something arriving is not an
		 * account of anything.
		 */
		void connectionCheckFailed();

		/**
		 * A server outside measured the advertised port and said what it found.
		 *
		 * Above everything inferred, because it is not an inference. A peer's
		 * refusal and a hub's failed check are accounts of an attempt this client
		 * never saw, made for reasons of their own; this is the outcome of an
		 * attempt against the published port by a party that was asked to make it
		 * and to say what happened. Still below traffic that arrived, which is not
		 * an account of anything at all.
		 *
		 * @param tcp what was found for the peer port, or State::Unknown where it
		 *        was not tested. Unknown decides nothing and leaves whatever else
		 *        is known to answer, so a run covering one family or one protocol
		 *        does not erase what is known about the other.
		 * @param udp the same for datagrams. This is the only thing that can put
		 *        UDP at Unreachable, because it is the only party that can be
		 *        asked to send a datagram and then asked whether it did - see
		 *        udp().
		 * @param observedAt when the measurement was taken and not when it got
		 *        here, so that a verdict handed over late expires on time.
		 *
		 * Ignored while nothing is advertised: a measurement is about an endpoint,
		 * and until one is announced there is none for it to be about.
		 */
		void measured(State tcp, State udp, time_t observedAt);

		/**
		 * What is known about the peer port, weighing the evidence in one order.
		 *
		 * A connection that arrived comes first, because it is proof rather than a
		 * claim: a measurement that reached nothing cannot unsay a peer that got
		 * through, whatever the measurement makes of the port afterwards.
		 *
		 * Then a measurement that has not expired, because an observation made
		 * from outside answers the question directly where everything below it
		 * only infers an answer from what other people say happened to them.
		 *
		 * Then a hub's failed check, and then enough peers refusing - the
		 * inferences, in the order described above.
		 */
		State tcp() const;

		/**
		 * What is known about the UDP port, in the same order.
		 *
		 * A datagram that arrived outranks a measurement for the reason a
		 * connection does. Below that a measurement is the only thing that can
		 * decide this at all: a peer that cannot reach our UDP port has nothing to
		 * report it with, so no quantity of peers saying nothing adds up to a
		 * verdict, while a server that was asked to send a datagram can be asked
		 * afterwards whether it did.
		 */
		State udp() const;

		/** How many distinct peers have reported a failure. */
		size_t reporters() const { return refused_by.size(); }

		/** Everything forgotten, as if nothing had ever been advertised. */
		void forget();

	private:
		/**
		 * Whether the measurement is recent enough to be about this network.
		 *
		 * One rule covers both a measurement that has expired and one that was
		 * never taken, which is the intent: an expired verdict counts for exactly
		 * as much as no verdict, and a measured_at left at the epoch is old.
		 */
		bool measurementApplies() const;

		std::string address;
		uint16_t port = 0;
		bool advertised = false;

		bool inbound_connection = false;
		bool check_failed = false;
		bool inbound_datagram = false;
		std::set<std::string> refused_by;

		State measured_tcp = State::Unknown;
		State measured_udp = State::Unknown;
		time_t measured_at = 0;
};

}

#endif // HAVE_QUICKDC_REACHABILITY_H
