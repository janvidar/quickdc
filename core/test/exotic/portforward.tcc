/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * QuickDC::PortForward against an internet gateway that only exists for the
 * test, on a loopback port the kernel picks.
 *
 * Driven through start(port, location) rather than the searching form, because a
 * test cannot rely on what answers a multicast search on the machine it runs on -
 * and because that is also the entry point a caller uses when it has the
 * gateway's address from configuration.
 *
 * The cases that matter are the ones where the gateway does not simply agree. A
 * router hands out a different external port than the one asked for, refuses a
 * lease, refuses the port outright, or grants TCP and refuses UDP - and the last
 * of those must not end with the client believing it is reachable, because an
 * active client that cannot receive a search reply looks to its user like a
 * network that returns nothing rather than like a setting that is wrong.
 */

#include "quickdc.h"
#include "network/portforward.h"

#include <samurai/eventloop.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/serversocket.h>
#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketaddress.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/url.h>

#include <chrono>
#include <map>
#include <memory>
#include <stdio.h>
#include <string>
#include <utility>
#include <vector>

static std::unique_ptr<Samurai::EventLoop> loop;

EXO_SETUP({
	loop = std::make_unique<Samurai::EventLoop>();
	return loop->isValid() ? 1 : 0;
});

EXO_TEARDOWN({
	loop.reset();
	return 1;
});

static const int TIMEOUT_MS = 4000;

template<typename Fn>
static bool pump(Fn done, int timeout_ms = TIMEOUT_MS)
{
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);

	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return done();
		loop->monitor().wait(10);
	}
	return true;
}
#include "fakegateway.h"


/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */

/*
 * What gets advertised to a hub.
 *
 * Both protocols ask this one function, so these are the cases that decide what
 * an ADC INF and an NMDC $MyINFO/$ConnectToMe carry. The forwarded port winning
 * over the configured one is the case the rest of this file exists for.
 */

/* ---- nothing asked for yet ---- */
EXO_TEST(portforward_a_port_forward_that_was_never_started_claims_nothing,
{
		Recorder events;
		QuickDC::PortForward forward(*loop, &events);

		EXO_ASSERT(!forward.isBusy());
		EXO_ASSERT(!forward.isForwarded());
		EXO_ASSERT(forward.getExternalPort() == 0);
		EXO_ASSERT(forward.getExternalAddress() == nullptr);
	
	return 1;
});

/*
	 * Version 1, so the port is asked for by name rather than left to the device.
	 * Both protocols have to be mapped before the port is reported, and the
	 * address is what the gateway said.
	 */
EXO_TEST(portforward_both_protocols_are_forwarded_and_the_address_reported,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.failed == 0);
		EXO_ASSERT(events.port == 1411);
		EXO_ASSERT(events.address == "203.0.113.7");
		EXO_ASSERT(forward.isForwarded());
		EXO_ASSERT(!forward.isBusy());
		EXO_ASSERT(forward.getExternalPort() == 1411);
		EXO_ASSERT(forward.getExternalAddress() != nullptr);

		/* TCP, then UDP, then the address - and nothing else. */
		EXO_ASSERT(gateway.count("AddPortMapping") == 2);
		EXO_ASSERT(gateway.count("GetExternalIPAddress") == 1);
		EXO_ASSERT(gateway.bodyOf("AddPortMapping", 0).find("<NewProtocol>TCP</NewProtocol>")
			!= std::string::npos);
		EXO_ASSERT(gateway.bodyOf("AddPortMapping", 1).find("<NewProtocol>UDP</NewProtocol>")
			!= std::string::npos);
	
	return 1;
});

/*
	 * The case the whole design turns on. A version 2 device may grant a port
	 * other than the one asked for, and the UDP mapping has to follow it: a peer
	 * is told one number, so a search reply arriving on another is lost with
	 * nothing anywhere to say why.
	 */
EXO_TEST(portforward_a_moved_external_port_is_carried_to_the_udp_mapping_too,
{
		FakeGateway gateway(*loop, IGD2);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway, 40001);

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.port == 40001);
		EXO_ASSERT(forward.getExternalPort() == 40001);

		/* The UDP mapping named the port TCP was actually granted, and still
		   forwards to the port this client is listening on. */
		const std::string udp = gateway.bodyOf("AddPortMapping", 0);
		EXO_ASSERT(gateway.count("AddAnyPortMapping") == 1);
		EXO_ASSERT(gateway.count("AddPortMapping") == 1);
		EXO_ASSERT(udp.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos);
		EXO_ASSERT(udp.find("<NewExternalPort>40001</NewExternalPort>") != std::string::npos);
		EXO_ASSERT(udp.find("<NewInternalPort>1411</NewInternalPort>") != std::string::npos);
	
	return 1;
});

/*
	 * 725 is common enough that samurai retries without a lease rather than
	 * reporting it. Asserted here because this component asks for a lease, and a
	 * client on such a router has to end up forwarded rather than passive.
	 */
EXO_TEST(portforward_a_router_that_refuses_a_lease_is_asked_again_without_one,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);
		gateway.script("AddPortMapping", fault(725));

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.failed == 0);
		EXO_ASSERT(events.port == 1411);

		/* The refused attempt, the one without a lease, and then UDP. */
		EXO_ASSERT(gateway.count("AddPortMapping") == 3);
		EXO_ASSERT(gateway.bodyOf("AddPortMapping", 1)
			.find("<NewLeaseDuration>0</NewLeaseDuration>") != std::string::npos);
	
	return 1;
});

/* ---- a port somebody else already holds ---- */
EXO_TEST(portforward_a_taken_port_is_walked_past_and_udp_follows,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);
		/* Refused once; the walk to the next port is what addAnyMapping promises. */
		gateway.script("AddPortMapping", fault(718));

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.port == 1412);

		/* And UDP followed it rather than staying on the port that was refused. */
		const std::string udp = gateway.bodyOf("AddPortMapping", 2);
		EXO_ASSERT(udp.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos);
		EXO_ASSERT(udp.find("<NewExternalPort>1412</NewExternalPort>") != std::string::npos);
	
	return 1;
});

/*
	 * Reported as a failure on purpose. A client with TCP forwarded and no UDP
	 * would be told it is active, be sent connections, and never see a reply to
	 * a search it made - which presents to its user as a network with nothing on
	 * it rather than as something to fix.
	 */
EXO_TEST(portforward_tcp_alone_is_not_reported_as_forwarded,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);
		gateway.script("AddPortMapping", ok(soapBody("AddPortMapping", "")));
		gateway.script("AddPortMapping", fault(718));

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.failed == 1);
		EXO_ASSERT(events.forwarded == 0);
		EXO_ASSERT(!forward.isForwarded());
		EXO_ASSERT(events.reason.find("718") != std::string::npos);
	
	return 1;
});

/* ---- a device that cannot forward anything ---- */
EXO_TEST(portforward_a_device_with_no_connection_service_is_refused,
{
		FakeGateway gateway(*loop, NOT_A_ROUTER);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.failed == 1);
		EXO_ASSERT(!forward.isForwarded());
		EXO_ASSERT(gateway.count("AddPortMapping") == 0);
	
	return 1;
});

/* ---- a description that cannot be read ---- */
EXO_TEST(portforward_a_description_that_cannot_be_fetched_is_refused,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		gateway.serve_description = false;

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());

		pump([&] { return events.done(); });

		EXO_ASSERT(events.failed == 1);
		EXO_ASSERT(!forward.isForwarded());
	
	return 1;
});

/* ---- releasing ---- */
EXO_TEST(portforward_stop_releases_both_mappings_and_reports_nothing,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		EXO_ASSERT(events.forwarded == 1);

		forward.stop();
		pump([&] { return gateway.count("DeletePortMapping") >= 2; });

		EXO_ASSERT(gateway.count("DeletePortMapping") == 2);
		EXO_ASSERT(!forward.isForwarded());
		EXO_ASSERT(forward.getExternalPort() == 0);
		/* Nothing further is reported: the caller asked for this. */
		EXO_ASSERT(events.forwarded == 1);
		EXO_ASSERT(events.failed == 0);
	
	return 1;
});

/* ---- asked twice ---- */
EXO_TEST(portforward_a_second_start_while_one_is_in_flight_is_refused,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());
		EXO_ASSERT(forward.isBusy());

		forward.start(1411, gateway.location());
		EXO_ASSERT(events.failed == 1);
		EXO_ASSERT(events.reason.find("already") != std::string::npos);

		pump([&] { return events.forwarded != 0; });
	
	return 1;
});

/*
	 * Nothing is reported and nothing is left registered with the monitor: the
	 * gateway and the mapper both hold a pointer back to the object being
	 * destroyed.
	 */
EXO_TEST(portforward_a_port_forward_destroyed_in_flight_reports_nothing,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		{
			QuickDC::PortForward forward(*loop, &events);
			forward.start(1411, gateway.location());
			pump([&] { return gateway.count("AddPortMapping") >= 1; });
		}

		pump([] { return false; }, 200);

		EXO_ASSERT(events.forwarded == 0);
		EXO_ASSERT(events.failed == 0);
	
	return 1;
});

/* Asked for, but nothing is listening, so there is nothing to reach. */
EXO_TEST(portforward_active_mode_without_a_listening_socket_is_still_passive,
{
		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, false, "10.0.0.5", "", 1411, nullptr);
		EXO_ASSERT(!e.active);
	
	return 1;
});

/* Asked for, listening, and not forwarding: what was configured. */
EXO_TEST(portforward_without_forwarding_the_configured_address_is_advertised,
{
		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", "", 1411, nullptr);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.address == "10.0.0.5");
		EXO_ASSERT(e.port == 1411);
	
	return 1;
});

/* Forwarding attempted and failed: still the configured values, because a
	   user who set up a mapping by hand is entitled to be believed. */
EXO_TEST(portforward_a_failed_forward_leaves_the_configured_address_alone,
{
		FakeGateway gateway(*loop, NOT_A_ROUTER);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		EXO_ASSERT(events.failed == 1);

		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", "", 1411, &forward);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.address == "10.0.0.5");
		EXO_ASSERT(e.port == 1411);
	
	return 1;
});

/*
	 * Forwarded, and the gateway moved the port. This is the case that would
	 * otherwise send every peer to a port with nothing behind it.
	 */
EXO_TEST(portforward_a_forwarded_mapping_advertises_the_granted_port_and_outside_address,
{
		FakeGateway gateway(*loop, IGD2);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway, 40001);

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		EXO_ASSERT(events.forwarded == 1);

		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", "", 1411, &forward);
		EXO_ASSERT(e.active);
		/* The gateway's outside address, not this host's on the LAN. */
		EXO_ASSERT(e.address == "203.0.113.7");
		EXO_ASSERT(e.port == 40001);
	
	return 1;
});

/* Released again: back to what was configured. */
EXO_TEST(portforward_a_released_mapping_falls_back_to_the_configured_port,
{
		FakeGateway gateway(*loop, IGD1);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway);

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		forward.stop();

		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", "", 1411, &forward);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.port == 1411);
	
	return 1;
});

/* ------------------------------------------------------------------------- */
/* An address something on the outside reported                              */
/* ------------------------------------------------------------------------- */

/*
 * A gateway asked for its outside address answers with the address of *its*
 * outside. Behind a carrier-grade NAT that is another private address, and a
 * client advertising it sends every peer somewhere that reaches nobody. Anything
 * that watched our packets arrive knows better, so it wins - an ADC hub reports
 * this, which is where the observed address comes from.
 */
EXO_TEST(portforward_an_observed_address_outranks_the_gateways_own,
{
		FakeGateway gateway(*loop, IGD2);
		if (!gateway.ready()) EXO_SKIP("could not listen on the loopback");
		agreeable(gateway, 40001);

		Recorder events;
		QuickDC::PortForward forward(*loop, &events);
		forward.start(1411, gateway.location());
		pump([&] { return events.done(); });
		EXO_ASSERT(events.forwarded == 1);

		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", "198.51.100.9", 1411, &forward);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.address == "198.51.100.9");

		/* Only the address. The gateway is the only thing that knows which port it
		   granted, and a hub cannot see that at all. */
		EXO_ASSERT(e.port == 40001);
	
	return 1;
});

/* And over the configured one, with no gateway in the picture at all - the case
   of a user who forwarded a port by hand and typed the wrong address. */
EXO_TEST(portforward_an_observed_address_outranks_the_configured_one,
{
		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", "198.51.100.9", 1411, nullptr);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.address == "198.51.100.9");
		EXO_ASSERT(e.port == 1411);
	
	return 1;
});

/* Nothing observed leaves the other two deciding, which is what every case above
   this section relies on. */
EXO_TEST(portforward_no_observed_address_changes_nothing,
{
		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(true, true, "10.0.0.5", "", 1411, nullptr);
		EXO_ASSERT(e.active);
		EXO_ASSERT(e.address == "10.0.0.5");
	
	return 1;
});

/* A passive client advertises nothing, whoever reported what. */
EXO_TEST(portforward_an_observed_address_does_not_make_a_passive_client_active,
{
		const QuickDC::PeerEndpoint e =
			QuickDC::peerEndpoint(false, true, "10.0.0.5", "198.51.100.9", 1411, nullptr);
		EXO_ASSERT(!e.active);
		EXO_ASSERT(e.address.empty());
	
	return 1;
});

/* ------------------------------------------------------------------------- */

/*
 * IPv6 is the shorter decision: no gateway, no translated port, and an address
 * the host carries itself. What is left is whether there is an endpoint at all.
 */

/* Asked for, but nothing is listening on this family - a host with no IPv6. */
EXO_TEST(portforward_ipv6_without_a_listening_socket_is_passive,
{
	const QuickDC::PeerEndpoint e =
		QuickDC::peerEndpoint6(true, false, "2001:db8::5", 1411);
	EXO_ASSERT(!e.active);
	return 1;
});

/* Not asked for at all, whatever is listening. */
EXO_TEST(portforward_ipv6_is_passive_when_active_mode_was_not_asked_for,
{
	const QuickDC::PeerEndpoint e =
		QuickDC::peerEndpoint6(false, true, "2001:db8::5", 1411);
	EXO_ASSERT(!e.active);
	return 1;
});

/*
 * Listening, but nobody has said where we are.
 *
 * Passive rather than advertised with a blank or a wildcard: an INF naming an
 * address peers cannot reach is worse than one claiming no IPv6 endpoint, since
 * the first has every peer trying and failing while the second has them use the
 * family that works.
 */
EXO_TEST(portforward_ipv6_is_passive_until_something_says_where_we_are,
{
	const QuickDC::PeerEndpoint e =
		QuickDC::peerEndpoint6(true, true, "", 1411);
	EXO_ASSERT(!e.active);
	EXO_ASSERT(e.address.empty());
	return 1;
});

/* And with all three, the address observed and the port bound. */
EXO_TEST(portforward_ipv6_advertises_the_address_observed_and_the_port_bound,
{
	const QuickDC::PeerEndpoint e =
		QuickDC::peerEndpoint6(true, true, "2001:db8::5", 1411);
	EXO_ASSERT(e.active);
	EXO_ASSERT(e.address == "2001:db8::5");
	EXO_ASSERT(e.port == 1411);
	return 1;
});
