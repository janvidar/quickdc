/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_REACHABILITY_H
#define HAVE_QUICKDC_REACHABILITY_H

#include "quickdc.h"

#include <set>
#include <string>

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
 * Evidence belongs to the endpoint it was gathered for. Advertise a different
 * address or port and everything learned about the old one says nothing about the
 * new one, so it is discarded - see advertising().
 *
 * Not detected: a mapping that dies while the endpoint stays the same, which is
 * what a lapsed UPnP lease looks like. Positive evidence is not aged out here,
 * so that case has to be caught by asking the gateway whether the mapping is
 * still there rather than by waiting for this to change its mind.
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

		State tcp() const;
		State udp() const;

		/** How many distinct peers have reported a failure. */
		size_t reporters() const { return refused_by.size(); }

		/** Everything forgotten, as if nothing had ever been advertised. */
		void forget();

	private:
		std::string address;
		uint16_t port = 0;
		bool advertised = false;

		bool inbound_connection = false;
		bool inbound_datagram = false;
		std::set<std::string> refused_by;
};

}

#endif // HAVE_QUICKDC_REACHABILITY_H
