/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * When to hold a gateway mapping.
 *
 * What these pin down is the sequence: a mapping is only shown useless by a check
 * that went through it, a useless one is let go and not asked for again until the
 * configuration changes, and one the gateway would not grant is asked for again
 * when a check says it is still needed.
 */

#include "quickdc.h"
#include "network/mappingpolicy.h"

namespace {

class Gateway : public QuickDC::MappingPolicy::Gateway {
	public:
		bool forwarded() const override { return state == State::Forwarded; }
		bool busy() const override { return state == State::Asking; }
		void start(uint16_t p) override { state = State::Asking; port = p; starts++; }
		void stop() override { state = State::Idle; stops++; }

		void grant() { state = State::Forwarded; }
		void refuse() { state = State::Idle; }

		enum class State { Idle, Asking, Forwarded } state = State::Idle;
		uint16_t port = 0;
		int starts = 0;
		int stops = 0;
};

}

EXO_TEST(mappingpolicy_a_wanted_mapping_is_asked_for_and_waited_on,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;

	EXO_ASSERT(policy.apply(gateway, true, 1411));
	EXO_ASSERT_EQ_INT(gateway.starts, 1);
	EXO_ASSERT_EQ_INT(gateway.port, 1411);

	/* Asked again while the ask is under way: nothing new. */
	policy.apply(gateway, true, 1411);
	EXO_ASSERT_EQ_INT(gateway.starts, 1);
	return 1;
});

EXO_TEST(mappingpolicy_one_no_longer_wanted_is_let_go,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;
	policy.apply(gateway, true, 1411);
	gateway.grant();

	EXO_ASSERT(!policy.apply(gateway, false, 1411));
	EXO_ASSERT_EQ_INT(gateway.stops, 1);
	EXO_ASSERT(!gateway.forwarded());
	return 1;
});

EXO_TEST(mappingpolicy_a_moved_port_is_mapped_again,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;
	policy.apply(gateway, true, 1411);
	gateway.grant();

	policy.apply(gateway, true, 1412);
	EXO_ASSERT_EQ_INT(gateway.stops, 1);
	EXO_ASSERT_EQ_INT(gateway.starts, 2);
	EXO_ASSERT_EQ_INT(gateway.port, 1412);
	return 1;
});

/* Passive, measured through the mapping: it did not help, so it goes and stays gone. */
EXO_TEST(mappingpolicy_a_mapping_measured_passive_through_it_is_released_for_good,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;
	policy.apply(gateway, true, 1411);
	gateway.grant();
	policy.granted();
	policy.checkStarted(gateway, false);

	policy.passive(gateway, true, 1411);
	EXO_ASSERT(policy.isUseless());
	EXO_ASSERT(!gateway.forwarded());
	EXO_ASSERT_EQ_INT(gateway.stops, 1);

	/* Another passive verdict does not have it asked for again. */
	policy.passive(gateway, true, 1411);
	policy.apply(gateway, true, 1411);
	EXO_ASSERT_EQ_INT(gateway.starts, 1);
	return 1;
});

/* A new configuration is a new chance for a mapping that did not help before. */
EXO_TEST(mappingpolicy_a_changed_configuration_tries_the_mapping_again,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;
	policy.apply(gateway, true, 1411);
	gateway.grant();
	policy.granted();
	policy.checkStarted(gateway, false);
	policy.passive(gateway, true, 1411);
	EXO_ASSERT(policy.isUseless());

	policy.configurationChanged();
	EXO_ASSERT(policy.apply(gateway, true, 1411));
	EXO_ASSERT_EQ_INT(gateway.starts, 2);
	return 1;
});

/*
 * Passive from a check that started before the mapping existed says nothing about
 * the mapping, so it stays - the next check, through it, is what can tell.
 */
EXO_TEST(mappingpolicy_a_verdict_from_before_the_mapping_does_not_release_it,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;
	policy.apply(gateway, true, 1411);
	policy.checkStarted(gateway, false);
	gateway.grant();
	policy.granted();

	policy.passive(gateway, true, 1411);
	EXO_ASSERT(!policy.isUseless());
	EXO_ASSERT(gateway.forwarded());
	EXO_ASSERT_EQ_INT(gateway.stops, 0);
	return 1;
});

/* The IPv6 run does not go through the gateway, so it tests no mapping. */
EXO_TEST(mappingpolicy_an_ipv6_check_does_not_test_the_mapping,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;
	policy.apply(gateway, true, 1411);
	gateway.grant();
	policy.granted();
	policy.checkStarted(gateway, true);

	policy.passive(gateway, true, 1411);
	EXO_ASSERT(!policy.isUseless());
	EXO_ASSERT(gateway.forwarded());
	return 1;
});

/* Refused before, still needed: asked for again rather than given up on. */
EXO_TEST(mappingpolicy_a_refused_mapping_is_asked_for_again_when_still_needed,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;
	policy.apply(gateway, true, 1411);
	gateway.refuse();
	policy.refused();

	EXO_ASSERT(policy.passive(gateway, true, 1411));
	EXO_ASSERT_EQ_INT(gateway.starts, 2);
	return 1;
});

/* A verdict is not consent: where the configuration wants no mapping, none is asked for. */
EXO_TEST(mappingpolicy_passive_asks_for_nothing_the_configuration_does_not_want,
{
	Gateway gateway;
	QuickDC::MappingPolicy policy;

	EXO_ASSERT(!policy.passive(gateway, false, 1411));
	EXO_ASSERT_EQ_INT(gateway.starts, 0);
	return 1;
});
