/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * What this client knows about being reachable from outside.
 *
 * The whole point is that it never asks the network anything: reachability cannot
 * be established except by traffic arriving unsolicited, and this client already
 * sends what provokes it. So the record is pure bookkeeping over evidence handed
 * to it, and every case here is about which evidence is allowed to decide what.
 *
 * Two properties carry the weight. A connection that arrived outranks any number
 * of peers saying they could not connect, because a refusal is a peer's account of
 * its own failure and an arrival is not an account of anything. And a refusal has
 * to come from several distinct peers, because the reports are written by whoever
 * is on the hub - a single one is as likely to be their firewall as ours, and one
 * peer must not be able to argue us into being passive by repeating itself.
 *
 * A measurement sits between the two. It is not asked for by this class either,
 * but it is an observation from outside rather than somebody's account of their own
 * failure, so it settles what the reports can only suggest - and it is the one
 * thing that can decide UDP against. It still loses to traffic that arrived, and it
 * stops counting once it is old enough to be about a network this client may have
 * left, which is the case the ages here are about.
 */

#include "quickdc.h"
#include "network/reachability.h"

#include <string>
#include <time.h>

using State = QuickDC::Reachability::State;

/* ------------------------------------------------------------------------- */
/* Nothing observed                                                          */
/* ------------------------------------------------------------------------- */

EXO_TEST(reachability_nothing_observed_is_not_a_verdict,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);
	EXO_ASSERT_EQ_UINT(r.reporters(), 0u);
	return 1;
});

/* And before anything has even been advertised. */
EXO_TEST(reachability_starts_knowing_nothing,
{
	QuickDC::Reachability r;
	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Something arrived                                                         */
/* ------------------------------------------------------------------------- */

EXO_TEST(reachability_an_inbound_connection_settles_tcp,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.inboundConnection();

	EXO_ASSERT(r.tcp() == State::Reachable);

	/* And says nothing about UDP, which is a separate mapping. */
	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

EXO_TEST(reachability_an_inbound_datagram_settles_udp,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.inboundDatagram();

	EXO_ASSERT(r.udp() == State::Reachable);
	EXO_ASSERT(r.tcp() == State::Unknown);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Peers reporting that they could not connect                               */
/* ------------------------------------------------------------------------- */

/*
 * One report is not enough, and neither is the same peer several times. A peer
 * that could force this on its own would have a cheap way of making us passive.
 */
EXO_TEST(reachability_one_peer_cannot_decide_it,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < 20; n++)
	{
		r.connectionRefused("CIDONE");
		if (r.tcp() != State::Unknown)
			EXO_FAIL("one peer repeating itself decided the question");
	}

	EXO_ASSERT_EQ_UINT(r.reporters(), 1u);
	return 1;
});

/* Enough distinct peers do decide it. */
EXO_TEST(reachability_enough_distinct_peers_decide_it,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < QUICKDC_REACHABILITY_REFUSALS; n++)
	{
		EXO_ASSERT(r.tcp() == State::Unknown);
		r.connectionRefused("CID" + std::to_string(n));
	}

	EXO_ASSERT(r.tcp() == State::Unreachable);
	return 1;
});

/* One short of the threshold is still not a verdict. */
EXO_TEST(reachability_one_short_of_the_threshold_decides_nothing,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < QUICKDC_REACHABILITY_REFUSALS - 1; n++)
		r.connectionRefused("CID" + std::to_string(n));

	EXO_ASSERT(r.tcp() == State::Unknown);
	return 1;
});

/* A report from nobody is not a report. */
EXO_TEST(reachability_a_refusal_from_nobody_is_ignored,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < 20; n++) r.connectionRefused("");

	EXO_ASSERT_EQ_UINT(r.reporters(), 0u);
	EXO_ASSERT(r.tcp() == State::Unknown);
	return 1;
});

/* The set of reporters is bounded: it is fed by whoever is on the hub. */
EXO_TEST(reachability_the_reporters_remembered_are_bounded,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < QUICKDC_REACHABILITY_MAX_REPORTERS * 4; n++)
		r.connectionRefused("CID" + std::to_string(n));

	EXO_ASSERT(r.reporters() <= QUICKDC_REACHABILITY_MAX_REPORTERS);

	/* And the verdict still stands - bounding the memory must not undo it. */
	EXO_ASSERT(r.tcp() == State::Unreachable);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What arrived outranks what was claimed                                    */
/* ------------------------------------------------------------------------- */

/*
 * A connection that arrived is not an account of anything, so no number of peers
 * saying otherwise can overturn it.
 */
EXO_TEST(reachability_an_arrival_outranks_any_number_of_refusals,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.inboundConnection();

	for (int n = 0; n < QUICKDC_REACHABILITY_REFUSALS * 4; n++)
		r.connectionRefused("CID" + std::to_string(n));

	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/* In either order. */
EXO_TEST(reachability_an_arrival_after_refusals_still_outranks_them,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < QUICKDC_REACHABILITY_REFUSALS * 4; n++)
		r.connectionRefused("CID" + std::to_string(n));
	EXO_ASSERT(r.tcp() == State::Unreachable);

	r.inboundConnection();
	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/*
 * UDP is never reported unreachable. A peer that cannot reach our UDP port has no
 * datagram it could send to say so, which is the whole difficulty - so silence is
 * the only symptom, and silence is not proof.
 */
EXO_TEST(reachability_udp_is_never_decided_against,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < QUICKDC_REACHABILITY_REFUSALS * 4; n++)
		r.connectionRefused("CID" + std::to_string(n));

	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* Evidence belongs to the endpoint it was gathered for                      */
/* ------------------------------------------------------------------------- */

EXO_TEST(reachability_a_new_port_discards_what_was_learned,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.inboundConnection();
	r.inboundDatagram();
	EXO_ASSERT(r.tcp() == State::Reachable);

	/* The gateway granted something else, so nothing known applies. */
	r.advertising("198.51.100.9", 40001);
	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

EXO_TEST(reachability_a_new_address_discards_what_was_learned,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < QUICKDC_REACHABILITY_REFUSALS; n++)
		r.connectionRefused("CID" + std::to_string(n));
	EXO_ASSERT(r.tcp() == State::Unreachable);

	/* A different network, so the peers that could not reach us said nothing
	   about where we are now. */
	r.advertising("203.0.113.7", 1411);
	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT_EQ_UINT(r.reporters(), 0u);
	return 1;
});

/* The same endpoint announced again keeps everything: an announcement happens
   whenever anything else about this client changes. */
EXO_TEST(reachability_readvertising_the_same_endpoint_keeps_what_is_known,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.inboundConnection();

	for (int n = 0; n < 10; n++) r.advertising("198.51.100.9", 1411);

	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/* And forgetting is forgetting. */
EXO_TEST(reachability_forgetting_leaves_nothing_behind,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.inboundConnection();
	r.inboundDatagram();
	r.connectionRefused("CIDONE");
	r.connectionCheckFailed();

	r.forget();

	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);
	EXO_ASSERT_EQ_UINT(r.reporters(), 0u);
	return 1;
});

/* Every state has a name, so a diagnostic never prints a number. */
EXO_TEST(reachability_every_state_can_be_named,
{
	EXO_ASSERT(std::string(QuickDC::Reachability::toString(State::Unknown)) == "not known");
	EXO_ASSERT(std::string(QuickDC::Reachability::toString(State::Reachable)) == "reachable");
	EXO_ASSERT(std::string(QuickDC::Reachability::toString(State::Unreachable)) == "unreachable");
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What a hub says about a connection it made itself                         */
/* ------------------------------------------------------------------------- */

/*
 * A hub that tried the published port and got nothing decides it on its own, where
 * three peers are needed. The two are not the same kind of statement: a peer's report
 * arrives through the hub quoting a token anyone on the hub saw, and describes a
 * failure only that peer witnessed, while this is a party that made the attempt itself
 * and said so over the connection we authenticated to it.
 */
EXO_TEST(reachability_one_hub_that_could_not_connect_decides_it,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	EXO_ASSERT(r.tcp() == State::Unknown);
	r.connectionCheckFailed();
	EXO_ASSERT(r.tcp() == State::Unreachable);
	return 1;
});

/*
 * But a connection that arrived still outranks it. The check may have run before a
 * port mapping came up, and something that arrived is not an account of anything.
 */
EXO_TEST(reachability_a_connection_that_arrived_outranks_a_failed_check,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.connectionCheckFailed();
	EXO_ASSERT(r.tcp() == State::Unreachable);

	r.inboundConnection();
	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/* In either order: a check that fails after a connection arrived changes nothing. */
EXO_TEST(reachability_a_failed_check_does_not_undo_a_connection_that_arrived,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.inboundConnection();
	r.connectionCheckFailed();

	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/* It says nothing about UDP, which nothing can speak against. */
EXO_TEST(reachability_a_failed_tcp_check_says_nothing_about_udp,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.connectionCheckFailed();

	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

/*
 * And it belongs to the endpoint it was gathered for. A new address or port is one
 * nothing has been checked about.
 */
EXO_TEST(reachability_a_failed_check_is_forgotten_when_the_endpoint_changes,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.connectionCheckFailed();
	EXO_ASSERT(r.tcp() == State::Unreachable);

	r.advertising("198.51.100.9", 4111);
	EXO_ASSERT(r.tcp() == State::Unknown);

	r.connectionCheckFailed();
	r.advertising("203.0.113.7", 4111);
	EXO_ASSERT(r.tcp() == State::Unknown);
	return 1;
});

/* Nothing has been advertised, so there is no endpoint for this to be about. */
EXO_TEST(reachability_a_failed_check_before_anything_is_advertised_is_ignored,
{
	QuickDC::Reachability r;

	r.connectionCheckFailed();

	EXO_ASSERT(r.tcp() == State::Unknown);
	return 1;
});

/* Said twice is said once. */
EXO_TEST(reachability_a_failed_check_repeated_changes_nothing,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < 10; n++) r.connectionCheckFailed();

	EXO_ASSERT(r.tcp() == State::Unreachable);
	EXO_ASSERT_EQ_UINT(r.reporters(), 0u);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What a server outside measured                                            */
/* ------------------------------------------------------------------------- */

/*
 * A measurement is an observation from outside rather than an inference from what
 * other people report, so it decides the question in either direction - and unlike
 * everything else here, that includes deciding UDP against. What it cannot do is
 * contradict traffic that arrived, and it stops counting once it is old enough to
 * be describing a network this client may have left.
 */

static time_t reachability_just_now() { return time(0); }

static time_t reachability_too_long_ago()
{
	return time(0) - QUICKDC_REACHABILITY_MEASUREMENT_AGE - 1;
}

EXO_TEST(reachability_a_measurement_decides_tcp_both_ways,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.measured(State::Reachable, State::Unknown, reachability_just_now());
	EXO_ASSERT(r.tcp() == State::Reachable);

	r.measured(State::Unreachable, State::Unknown, reachability_just_now());
	EXO_ASSERT(r.tcp() == State::Unreachable);
	return 1;
});

/* A run that tested neither port says nothing, and must not silence what does. */
EXO_TEST(reachability_a_measurement_of_nothing_decides_nothing,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.connectionCheckFailed();

	r.measured(State::Unknown, State::Unknown, reachability_just_now());

	EXO_ASSERT(r.tcp() == State::Unreachable);
	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

/*
 * A connection that got through outranks a measurement that could not get one
 * through: one attempt failing is not an account of the ones that succeeded.
 */
EXO_TEST(reachability_a_connection_that_arrived_outranks_a_passive_measurement,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.measured(State::Unreachable, State::Unknown, reachability_just_now());
	EXO_ASSERT(r.tcp() == State::Unreachable);

	r.inboundConnection();
	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/* In either order: measuring nothing after a connection arrived changes nothing. */
EXO_TEST(reachability_a_passive_measurement_does_not_undo_a_connection_that_arrived,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.inboundConnection();
	r.measured(State::Unreachable, State::Unknown, reachability_just_now());

	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/* But it outranks the hub that could not connect, which only tried once too. */
EXO_TEST(reachability_a_measurement_outranks_a_failed_check,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.connectionCheckFailed();
	EXO_ASSERT(r.tcp() == State::Unreachable);

	r.measured(State::Reachable, State::Unknown, reachability_just_now());
	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/* And the peers who said they could not get in, however many of them there are. */
EXO_TEST(reachability_a_measurement_outranks_the_refusal_threshold,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	for (int n = 0; n < QUICKDC_REACHABILITY_REFUSALS * 4; n++)
		r.connectionRefused("CID" + std::to_string(n));
	EXO_ASSERT(r.tcp() == State::Unreachable);

	r.measured(State::Reachable, State::Unknown, reachability_just_now());
	EXO_ASSERT(r.tcp() == State::Reachable);
	return 1;
});

/*
 * Old enough and it is ignored entirely, as though it had never been taken: a
 * laptop that moved network holds a verdict about a network it is no longer on.
 */
EXO_TEST(reachability_an_expired_measurement_decides_nothing,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.measured(State::Unreachable, State::Unreachable, reachability_too_long_ago());

	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

/* Which means it stops outranking anything, rather than being weighed lower. */
EXO_TEST(reachability_an_expired_measurement_leaves_the_inference_alone,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.measured(State::Reachable, State::Unknown, reachability_too_long_ago());
	r.connectionCheckFailed();

	EXO_ASSERT(r.tcp() == State::Unreachable);
	return 1;
});

/*
 * UDP can be decided against, but only here. A server asked to send a datagram
 * can be asked afterwards whether it did, which is what nobody else can answer.
 */
EXO_TEST(reachability_a_measurement_decides_udp_both_ways,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.measured(State::Unknown, State::Unreachable, reachability_just_now());
	EXO_ASSERT(r.udp() == State::Unreachable);

	/* And says nothing about TCP, which is a separate mapping. */
	EXO_ASSERT(r.tcp() == State::Unknown);

	r.measured(State::Unknown, State::Reachable, reachability_just_now());
	EXO_ASSERT(r.udp() == State::Reachable);
	return 1;
});

/* A datagram that arrived outranks it, for the reason a connection does. */
EXO_TEST(reachability_a_datagram_that_arrived_outranks_a_passive_measurement,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.measured(State::Unknown, State::Unreachable, reachability_just_now());
	EXO_ASSERT(r.udp() == State::Unreachable);

	r.inboundDatagram();
	EXO_ASSERT(r.udp() == State::Reachable);

	/* In either order. */
	r.measured(State::Unknown, State::Unreachable, reachability_just_now());
	EXO_ASSERT(r.udp() == State::Reachable);
	return 1;
});

/* A measurement belongs to the endpoint it was taken against, like everything
   else here: it answered for a port that is no longer the one being published. */
EXO_TEST(reachability_a_new_endpoint_discards_a_measurement,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);

	r.measured(State::Reachable, State::Reachable, reachability_just_now());
	EXO_ASSERT(r.tcp() == State::Reachable);

	r.advertising("198.51.100.9", 40001);
	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);

	r.measured(State::Reachable, State::Reachable, reachability_just_now());
	r.advertising("203.0.113.7", 40001);
	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

/* The same endpoint announced again keeps it, as it keeps everything else. */
EXO_TEST(reachability_readvertising_the_same_endpoint_keeps_a_measurement,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.measured(State::Unreachable, State::Unreachable, reachability_just_now());

	for (int n = 0; n < 10; n++) r.advertising("198.51.100.9", 1411);

	EXO_ASSERT(r.tcp() == State::Unreachable);
	EXO_ASSERT(r.udp() == State::Unreachable);
	return 1;
});

/* And forgetting forgets it too. */
EXO_TEST(reachability_forgetting_discards_a_measurement,
{
	QuickDC::Reachability r;
	r.advertising("198.51.100.9", 1411);
	r.measured(State::Unreachable, State::Unreachable, reachability_just_now());
	EXO_ASSERT(r.tcp() == State::Unreachable);

	r.forget();

	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});

/* Nothing has been advertised, so there is no endpoint for this to be about. */
EXO_TEST(reachability_a_measurement_before_anything_is_advertised_is_ignored,
{
	QuickDC::Reachability r;

	r.measured(State::Reachable, State::Reachable, reachability_just_now());

	EXO_ASSERT(r.tcp() == State::Unknown);
	EXO_ASSERT(r.udp() == State::Unknown);
	return 1;
});
