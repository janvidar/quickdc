/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/networkwatch.h"
#include "network/localaddresses.h"

#include <samurai/eventloop.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/timer.h>

#include <chrono>
#include <string>

using namespace std::chrono_literals;

static void run(Samurai::EventLoop& loop, std::chrono::milliseconds span)
{
	const auto until = std::chrono::steady_clock::now() + span;
	while (std::chrono::steady_clock::now() < until)
	{
		loop.monitor().wait(5);
		loop.timers().process();
	}
}

static const QuickDC::NetworkWatch::Timing FAST{20ms, 60ms};

EXO_TEST(networkwatch_addresses_that_change_are_told_once_settled,
{
	Samurai::EventLoop loop;
	std::string addresses = "10.0.0.5";
	int told = 0;
	QuickDC::NetworkWatch watch(loop, [&] { told++; }, [&] { return addresses; }, FAST);

	run(loop, 100ms);
	EXO_ASSERT(told == 0);

	addresses = "10.0.0.5 192.0.2.7";
	run(loop, 50ms);
	addresses = "192.0.2.7";
	run(loop, 250ms);
	EXO_ASSERT(told == 1);
	return 1;
});

EXO_TEST(networkwatch_a_change_while_offline_is_not_told,
{
	Samurai::EventLoop loop;
	int told = 0;
	QuickDC::NetworkWatch watch(loop, [&] { told++; }, [] { return std::string("10.0.0.5"); }, FAST);

	watch.changed(false);
	run(loop, 200ms);
	EXO_ASSERT(told == 0);

	watch.changed(true);
	run(loop, 200ms);
	EXO_ASSERT(told == 1);
	return 1;
});

EXO_TEST(networkwatch_an_address_appearing_changing_or_going_is_a_change,
{
	const std::string before = QuickDC::fingerprintOf({"192.168.1.10"});
	EXO_ASSERT(QuickDC::fingerprintOf({"192.168.1.11"}) != before);
	EXO_ASSERT(QuickDC::fingerprintOf({"192.168.1.10", "10.8.0.2"}) != before);
	EXO_ASSERT(QuickDC::fingerprintOf({}) != before);
	return 1;
});

EXO_TEST(networkwatch_loopback_and_link_local_are_not_a_change,
{
	EXO_ASSERT(QuickDC::fingerprintOf({"127.0.0.1", "::1", "169.254.3.4", "fe80::1", "192.168.1.10"})
	           == QuickDC::fingerprintOf({"192.168.1.10"}));
	EXO_ASSERT(QuickDC::fingerprintOf({"127.0.0.1", "fe80::1"}).empty());
	return 1;
});

EXO_TEST(networkwatch_order_and_duplicates_are_not_a_change,
{
	EXO_ASSERT(QuickDC::fingerprintOf({"192.168.1.10", "10.8.0.2", "2001:db8::5"})
	           == QuickDC::fingerprintOf({"2001:db8::5", "192.168.1.10", "10.8.0.2"}));
	EXO_ASSERT(QuickDC::fingerprintOf({"192.168.1.10", "192.168.1.10"})
	           == QuickDC::fingerprintOf({"192.168.1.10"}));
	return 1;
});

EXO_TEST(networkwatch_this_hosts_fingerprint_leaves_out_loopback,
{
	EXO_ASSERT(QuickDC::addressFingerprint().find("127.0.0.1") == std::string::npos);
	return 1;
});
