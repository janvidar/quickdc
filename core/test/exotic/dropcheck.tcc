/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * QuickDC::DropCheck, asked only what can be answered without a DROP server.
 *
 * A run needs an HTTPS control plane on the other end, so nothing here starts
 * one - and what is left is the part that carries the risk anyway. DropCheck
 * lends the measurement two sockets this client already listens on, the peer
 * port and the search port, and both of those are open to whoever finds them.
 * The window in which they answer a stranger is the whole security boundary, it
 * belongs to this class alone, and it is decided before any byte is parsed.
 *
 * The UDP half is what makes it matter. A datagram's source address is whatever
 * the sender wrote, so a client that replies to an unauthenticated probe is a
 * reflector an attacker aims by spoofing that address. Refusing while no run is
 * in flight is what closes that, and it has to hold for a datagram that is the
 * real thing, because the real thing is what an attacker sends. So each refusal
 * below is paired with the library answering the very same bytes: that is what
 * makes the case about the run not being open rather than about bytes nobody
 * could have made sense of.
 */

#include "quickdc.h"
#include "api/dropverdict.h"
#include "network/dropcheck.h"
#include "network/reachability.h"

#include <drop/responder.h>
#include <drop/stun.h>
#include <drop/types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/* ------------------------------------------------------------------------- */

class Recorder : public QuickDC::DropCheckListener
{
	public:
		int finished = 0;
		int failed = 0;
		std::string reason;

		void EventDropCheckFinished(const QuickDC::DropVerdict&) override
		{
			finished++;
		}

		void EventDropCheckFailed(const char* why) override
		{
			failed++;
			reason = why ? why : "(null)";
		}
};

/* The line a DROP server sends to the peer port, terminator and all. */
static const char* CHALLENGE = "DROP/1 CHALLENGE 4a1f9c2e\n";

/* The credentials of a session that is not this client's. */
static const char* PROBE_USER = "0f1e2d3c:quickdc";
static const char* PROBE_PASS = "not-this-clients-session";

/**
 * A Binding Request as a session's own probe would arrive: username, message
 * integrity over the password, and a fingerprint.
 *
 * Built with the library's own encoder rather than by hand so that it is
 * complete - a hand-written header would be refused for being empty, which
 * would prove nothing about the rule under test.
 */
static std::vector<uint8_t> authenticatedProbe()
{
	const Drop::Stun::TxId txid{
		0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a,
		0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4
	};

	Drop::Stun::Message request = Drop::Stun::Message::request(txid);
	request.addString(Drop::Stun::Attr::Username, PROBE_USER);

	const std::string_view password(PROBE_PASS);
	const std::span<const uint8_t> key(
		reinterpret_cast<const uint8_t*>(password.data()), password.size());

	request.addMessageIntegrity(key);
	request.addFingerprint();

	return request.encode();
}

/* ------------------------------------------------------------------------- */
/* Nothing has been measured                                                 */
/* ------------------------------------------------------------------------- */

EXO_TEST(dropcheck_a_check_that_never_ran_is_idle_and_has_no_verdict,
{
	Recorder events;
	QuickDC::DropCheck check(&events);

	EXO_ASSERT(!check.isBusy());
	EXO_ASSERT(!check.verdict().ran);
	EXO_ASSERT(check.verdict().mode == QuickDC::DropMode::Unknown);
	EXO_ASSERT(check.verdict().port == 0);
	EXO_ASSERT(events.finished == 0);
	EXO_ASSERT(events.failed == 0);
	return 1;
});

/* Nothing was abandoned, so there is nothing to report about it either. */
EXO_TEST(dropcheck_cancelling_a_check_that_is_not_running_changes_nothing,
{
	Recorder events;
	QuickDC::DropCheck check(&events);

	for (int n = 0; n < 10; n++) check.cancel();

	EXO_ASSERT(!check.isBusy());
	EXO_ASSERT(!check.verdict().ran);

	EXO_ASSERT(events.finished == 0);
	EXO_ASSERT(events.failed == 0);
	return 1;
});

/* ------------------------------------------------------------------------- */
/* What the shared sockets answer while no run is in flight                  */
/* ------------------------------------------------------------------------- */

/*
 * The case this file exists for. A freshly constructed check has no session, so
 * a probe arriving on the search port comes from somebody whose address nobody
 * proved, and the answer to it is silence rather than an error - an error is
 * still a reply, and a reply is what gets reflected.
 *
 * The probe is a complete authenticated Binding Request, and it is shown to be both
 * of those before it is refused: first the shape a socket carrying STUN and search
 * results demultiplexes on - top two bits zero, and the magic cookie in the second
 * word - and then answerable, because a responder holding the credentials it was
 * signed with replies to these exact bytes. That is what makes the refusals below a
 * statement about the run not being open rather than about bytes nobody could have
 * made sense of.
 *
 * The refusal then holds however the sender describes itself, on either family,
 * since the description is the part that cannot be trusted.
 */
EXO_TEST(dropcheck_an_unarmed_check_answers_no_stun_probe,
{
	Recorder events;
	QuickDC::DropCheck check(&events);

	const std::vector<uint8_t> probe = authenticatedProbe();
	std::vector<uint8_t> reply;

	EXO_ASSERT(Drop::Stun::Message::looksLikeStun(probe));
	EXO_ASSERT((probe[0] & 0xc0) == 0);
	EXO_ASSERT(probe[4] == 0x21 && probe[5] == 0x12);
	EXO_ASSERT(probe[6] == 0xa4 && probe[7] == 0x42);

	Drop::Stun::Endpoint from;
	from.address = "198.51.100.9";
	from.port = 1411;
	EXO_ASSERT(Drop::handleStunProbe(probe, PROBE_USER, PROBE_PASS, from, reply));
	EXO_ASSERT(!reply.empty());
	reply.clear();

	EXO_ASSERT(!check.answerStunProbe(probe, "198.51.100.9", 1411, false, reply));
	EXO_ASSERT(reply.empty());

	EXO_ASSERT(!check.answerStunProbe(probe, "203.0.113.7", 40001, false, reply));
	EXO_ASSERT(!check.answerStunProbe(probe, "2001:db8::1", 1411, true, reply));
	EXO_ASSERT(reply.empty());
	return 1;
});

/*
 * The TCP half of the same rule. A client that echoes the challenge to anybody
 * tells strangers what it runs for nothing in return, so the window is closed
 * here too - even though a peer that completed a handshake has proven its
 * address and there is nothing to reflect.
 *
 * Paired the same way as the probe above: the line is a real challenge and the
 * library answers it, so the refusal that follows is about the arming gate. And it
 * holds whichever terminator arrived, or none.
 */
EXO_TEST(dropcheck_an_unarmed_check_answers_no_challenge,
{
	Recorder events;
	QuickDC::DropCheck check(&events);

	std::string response;

	EXO_ASSERT(Drop::TcpResponder::handleChallenge(CHALLENGE, response));
	EXO_ASSERT(!response.empty());
	response.clear();

	EXO_ASSERT(!check.answerChallenge(CHALLENGE, response));
	EXO_ASSERT(response.empty());

	EXO_ASSERT(!check.answerChallenge("DROP/1 CHALLENGE 4a1f9c2e\r\n", response));
	EXO_ASSERT(!check.answerChallenge("DROP/1 CHALLENGE 4a1f9c2e", response));
	EXO_ASSERT(response.empty());
	return 1;
});

/*
 * And the bytes that actually share that port are not challenges either, which
 * is what lets the protocol probe hand this everything it could not place.
 *
 * The bytes are the ones that really turn up there: an ADC client-to-client
 * greeting, the first bytes of the TLS ClientHello that ADCS opens with, and
 * nothing at all. None of them is a challenge to the library either, so a run
 * being open would not have changed any of the above.
 */
EXO_TEST(dropcheck_ordinary_peer_port_bytes_are_not_a_challenge,
{
	Recorder events;
	QuickDC::DropCheck check(&events);

	std::string response;

	EXO_ASSERT(!check.answerChallenge("CSUP ADBASE ADTIGR\n", response));

	EXO_ASSERT(!check.answerChallenge(std::string_view("\x16\x03\x01\x00\x9a\x01", 6),
	                                  response));

	EXO_ASSERT(!check.answerChallenge("", response));
	EXO_ASSERT(!check.answerChallenge("\n", response));

	EXO_ASSERT(response.empty());

	EXO_ASSERT(!Drop::TcpResponder::handleChallenge("CSUP ADBASE ADTIGR\n", response));
	EXO_ASSERT(!Drop::TcpResponder::handleChallenge("DROP/1 CHALLENGE ", response));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* A run that cannot be started                                              */
/* ------------------------------------------------------------------------- */

/*
 * Refused inside the call, before a client is built or a name is looked up. The
 * port measured is the one this client advertises and there is no substitute for
 * it: a check of some ephemeral port would be a true answer about a port nobody
 * will ever connect to.
 *
 * The reason has to be something a user can read, since it is shown beside the
 * connection settings it is about. The check is idle again afterwards, so the next
 * start is not refused as a second one. And there is still no verdict, because a
 * failure to measure says nothing about whether this client is reachable.
 */
EXO_TEST(dropcheck_a_start_without_a_peer_port_fails_at_once,
{
	Recorder events;
	QuickDC::DropCheck check(&events);

	check.start("https://ipv4.drop.example.org", Drop::Family::IPv4,
	            0, 0, std::nullopt);

	EXO_ASSERT(events.failed == 1);
	EXO_ASSERT(events.finished == 0);
	EXO_ASSERT(!events.reason.empty());
	EXO_ASSERT(!check.isBusy());
	EXO_ASSERT(!check.verdict().ran);
	return 1;
});

/* Named UDP port or not: it is the peer port that decides. */
EXO_TEST(dropcheck_a_start_without_a_peer_port_fails_at_once_with_a_udp_port_too,
{
	Recorder events;
	QuickDC::DropCheck check(&events);

	check.start("https://ipv4.drop.example.org", Drop::Family::IPv6,
	            0, 1411, std::nullopt);

	EXO_ASSERT(events.failed == 1);
	EXO_ASSERT(!check.isBusy());
	return 1;
});

/* A start that failed must not have armed anything on its way past. */
EXO_TEST(dropcheck_a_failed_start_leaves_nothing_armed,
{
	Recorder events;
	QuickDC::DropCheck check(&events);

	check.start("https://ipv4.drop.example.org", Drop::Family::IPv4,
	            0, 1411, std::nullopt);
	EXO_ASSERT(events.failed == 1);

	std::string response;
	std::vector<uint8_t> reply;

	EXO_ASSERT(!check.answerChallenge(CHALLENGE, response));
	EXO_ASSERT(!check.answerStunProbe(authenticatedProbe(), "198.51.100.9", 1411,
	                                  false, reply));
	EXO_ASSERT(response.empty());
	EXO_ASSERT(reply.empty());
	return 1;
});

/* ------------------------------------------------------------------------- */

/* Every mode has a name, so a diagnostic never prints a number. */
EXO_TEST(dropcheck_every_mode_can_be_named,
{
	EXO_ASSERT(std::string(QuickDC::toString(QuickDC::DropMode::Unknown)) == "not known");
	EXO_ASSERT(std::string(QuickDC::toString(QuickDC::DropMode::Active)) == "active");
	EXO_ASSERT(std::string(QuickDC::toString(QuickDC::DropMode::Passive)) == "passive");
	return 1;
});

/* ------------------------------------------------------------------------- */

/*
 * How long a measurement is worth keeping, which is what decides when another is
 * taken. The answer has to be the same one the reachability record reaches, or
 * the client would go on believing a verdict nothing intends to refresh, or keep
 * measuring one that is still being believed.
 */

static QuickDC::DropVerdict measuredAt(time_t when)
{
	QuickDC::DropVerdict verdict;
	verdict.ran = true;
	verdict.observedAt = when;
	return verdict;
}

/* Nothing measured is not a fresh measurement, whatever else the verdict holds. */
EXO_TEST(dropcheck_a_verdict_that_never_ran_is_never_fresh,
{
	const time_t now = 1000000;

	EXO_ASSERT(!QuickDC::dropMeasurementIsFresh(QuickDC::DropVerdict(), now));

	QuickDC::DropVerdict unrun = measuredAt(now);
	unrun.ran = false;
	EXO_ASSERT(!QuickDC::dropMeasurementIsFresh(unrun, now));

	/*
	 * Nor one that ran but recorded no time, which there is no judging.
	 *
	 * Asked close to the epoch, because that is the only place the answer turns
	 * on saying so: further out, an unset time is refused for being ancient
	 * rather than for being unset, and a check dropped here would go unnoticed
	 * until a clock made it matter.
	 */
	QuickDC::DropVerdict undated = measuredAt(0);
	EXO_ASSERT(!QuickDC::dropMeasurementIsFresh(undated, now));
	EXO_ASSERT(!QuickDC::dropMeasurementIsFresh(undated, 1));
	return 1;
});

/* Believed right up to the age, and not one second past it. */
EXO_TEST(dropcheck_a_measurement_is_believed_for_exactly_as_long_as_the_record_does,
{
	const time_t now = 1000000;
	const time_t age = QUICKDC_REACHABILITY_MEASUREMENT_AGE;

	EXO_ASSERT(QuickDC::dropMeasurementIsFresh(measuredAt(now), now));
	EXO_ASSERT(QuickDC::dropMeasurementIsFresh(measuredAt(now - age + 1), now));
	EXO_ASSERT(QuickDC::dropMeasurementIsFresh(measuredAt(now - age), now));
	EXO_ASSERT(!QuickDC::dropMeasurementIsFresh(measuredAt(now - age - 1), now));
	return 1;
});

/*
 * A measurement dated later than the moment it is judged from is not fresh.
 *
 * These are wall-clock times, so an operator correcting a clock, or a machine
 * resuming with a bad one, can leave one behind. Believing it would hold a stale
 * answer for however far the clock was out - and the further out, the longer.
 */
EXO_TEST(dropcheck_a_measurement_from_the_future_is_not_believed,
{
	const time_t now = 1000000;

	EXO_ASSERT(!QuickDC::dropMeasurementIsFresh(measuredAt(now + 1), now));
	EXO_ASSERT(!QuickDC::dropMeasurementIsFresh(measuredAt(now + 86400), now));
	return 1;
});

/* ------------------------------------------------------------------------- */

/*
 * Who hears about a verdict, which is the core and anything else that asked.
 *
 * What the core does with one - record the measurement, ask the gateway to
 * forward a port nothing reached, have the hubs republish - happens whether or
 * not a front end is drawing the same answer. A client that behaved one way with
 * a settings page open and another without it would be two different clients.
 */

/** Records what it was told, and when, relative to every other one of these. */
static int drop_order = 0;

class OrderedRecorder : public QuickDC::DropCheckListener
{
	public:
		int failed = 0;
		int failedAt = 0;

		void EventDropCheckFinished(const QuickDC::DropVerdict&) override { }

		void EventDropCheckFailed(const char*) override
		{
			failed++;
			failedAt = ++drop_order;
		}
};

/* Both are told, and the core first, so the record is written before it is read. */
EXO_TEST(dropcheck_a_front_end_watching_does_not_take_the_core_off_the_verdict,
{
	drop_order = 0;

	OrderedRecorder core;
	OrderedRecorder front;

	QuickDC::DropCheck check(&core);
	check.setObserver(&front);

	check.start("https://ipv4.drop.example.org", Drop::Family::IPv4,
	            0, 1411, std::nullopt);

	EXO_ASSERT(core.failed == 1);
	EXO_ASSERT(front.failed == 1);
	EXO_ASSERT(core.failedAt < front.failedAt);
	return 1;
});

/*
 * And the core keeps hearing once the front end has gone.
 *
 * The window closing is what takes the observer off, and a measurement taken
 * after that is still the client's own business.
 */
EXO_TEST(dropcheck_the_core_still_hears_a_verdict_after_the_front_end_goes,
{
	OrderedRecorder core;
	OrderedRecorder front;

	QuickDC::DropCheck check(&core);
	check.setObserver(&front);

	check.start("https://ipv4.drop.example.org", Drop::Family::IPv4,
	            0, 1411, std::nullopt);
	EXO_ASSERT(core.failed == 1 && front.failed == 1);

	check.setObserver(nullptr);
	EXO_ASSERT(check.getObserver() == nullptr);

	check.start("https://ipv4.drop.example.org", Drop::Family::IPv4,
	            0, 1411, std::nullopt);

	EXO_ASSERT(core.failed == 2);
	EXO_ASSERT(front.failed == 1);
	return 1;
});

/* A check nobody is watching still reports to the core, and does not crash. */
EXO_TEST(dropcheck_a_check_with_no_front_end_reports_to_the_core_alone,
{
	OrderedRecorder core;
	QuickDC::DropCheck check(&core);

	EXO_ASSERT(check.getObserver() == nullptr);

	check.start("https://ipv4.drop.example.org", Drop::Family::IPv4,
	            0, 1411, std::nullopt);

	EXO_ASSERT(core.failed == 1);
	return 1;
});
