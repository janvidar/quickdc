/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dropcheck.h"
#include "api/core.h"
#include "config/preferences.h"
#include "network/reachability.h"

#include <drop/responder.h>
#include <drop/stun.h>

#include <samurai/io/net/inetaddress.h>

#include <time.h>

namespace {

/**
 * One port's outcome, as something to decide on.
 *
 * Refused and Occupied are blocked rather than reached even though both mean a
 * packet arrived: the first says nothing was listening and the second that what
 * answered was not this client, and neither is a port a peer can be told to
 * connect to. Error is Unknown on purpose - the server could not run the probe,
 * which is a statement about the server.
 */
QuickDC::DropReach reachOf(Drop::Verdict verdict)
{
	switch (verdict)
	{
		case Drop::Verdict::Open:
			return QuickDC::DropReach::Reached;

		case Drop::Verdict::Occupied:
		case Drop::Verdict::Refused:
		case Drop::Verdict::Filtered:
		case Drop::Verdict::NoResponse:
			return QuickDC::DropReach::Blocked;

		case Drop::Verdict::Unknown:
		case Drop::Verdict::Error:
			return QuickDC::DropReach::Unknown;
	}

	return QuickDC::DropReach::Unknown;
}

/** The same, in the vocabulary the reachability record keeps. */
QuickDC::Reachability::State stateOf(QuickDC::DropReach reach)
{
	switch (reach)
	{
		case QuickDC::DropReach::Reached: return QuickDC::Reachability::State::Reachable;
		case QuickDC::DropReach::Blocked: return QuickDC::Reachability::State::Unreachable;
		case QuickDC::DropReach::Unknown: return QuickDC::Reachability::State::Unknown;
	}

	return QuickDC::Reachability::State::Unknown;
}

}

QuickDC::DropCheck::DropCheck(DropCheckListener* listener_) : listener(listener_)
{
}

QuickDC::DropCheck::~DropCheck()
{
	internal_disarm();
	client.reset();
}

void QuickDC::DropCheck::start(const std::string& baseUrl, Drop::Family family_,
                               uint16_t tcpPort, uint16_t udpPort,
                               const std::optional<Samurai::IO::Net::InetAddress>& localAddress)
{
	if (isBusy())
	{
		internal_fail("a reachability check is already running");
		return;
	}

	/*
	 * Before anything that can refuse, so that a refusal still names the family
	 * it was asked about. A caller deciding what to do next from the family of
	 * the run that just ended - measure the other one - would otherwise be told
	 * about the previous run, and on a client with no peer port at all would be
	 * sent back to the same family it had just been refused for, for ever.
	 */
	family = family_;

	if (!tcpPort)
	{
		internal_fail("there is no peer port to test");
		return;
	}

	tcp_port = tcpPort;
	udp_port = udpPort;
	want_udp = false;

	server = Drop::ServerInfo();
	whoami = Drop::Whoami();
	internal_disarm();

	Drop::Client::Options options;

	if (localAddress) options.localAddress = *localAddress;

	options.userAgent = PRODUCT "/" VERSION;

	client = std::make_unique<Drop::Client>(this, baseUrl, family, options);

	state = State::ServerInfo;
	client->requestServerInfo();
}

void QuickDC::DropCheck::cancel()
{
	if (!isBusy()) return;

	QDBG("Abandoning the reachability check");

	if (client) client->abort();
	state = State::Idle;
	internal_disarm();
}

void QuickDC::DropCheck::internal_disarm()
{
	session = Drop::Session();
	armed = false;
}

void QuickDC::DropCheck::internal_fail(const std::string& reason)
{
	state = State::Idle;
	internal_disarm();

	QERR("The reachability check could not be completed: %s", reason.c_str());

	if (listener) listener->EventDropCheckFailed(reason.c_str());
	if (observer) observer->EventDropCheckFailed(reason.c_str());
}

void QuickDC::DropCheck::internal_finish()
{
	state = State::Idle;
	internal_disarm();

	result.observedAt = time(0);
	result.ran = true;

	QDBG("Reachability: %s on %s port %d, seen at %s",
	     toString(result.mode), result.ipv6 ? "IPv6" : "IPv4",
	     static_cast<int>(result.port), result.publicAddress.c_str());

	/*
	 * Into its family's slot before anybody is told, so that a listener asking
	 * what is known - to decide whether the other family is still owed a run -
	 * is answered with this one rather than with what it replaced.
	 */
	(result.ipv6 ? held_v6 : held_v4) = result;

	const DropVerdict reported = result;

	if (listener) listener->EventDropCheckFinished(reported);
	if (observer) observer->EventDropCheckFinished(reported);
}

bool QuickDC::DropCheck::answerChallenge(std::string_view line, std::string& response) const
{
	if (!armed) return false;

	return Drop::TcpResponder::handleChallenge(line, response);
}

bool QuickDC::DropCheck::answerStunProbe(std::span<const uint8_t> datagram,
                                         const std::string& fromAddress,
                                         uint16_t fromPort, bool fromIsV6,
                                         std::vector<uint8_t>& reply) const
{
	if (!armed) return false;

	Drop::Stun::Endpoint from;
	from.address = fromAddress;
	from.port = fromPort;
	from.v6 = fromIsV6;

	return Drop::handleStunProbe(datagram, session.username, session.password,
	                             from, reply);
}

void QuickDC::DropCheck::EventDropServerInfo(Drop::Client*, const Drop::ServerInfo& info)
{
	if (state != State::ServerInfo) return;

	server = info;

	want_udp = udp_port && server.supports(family, "udp");

	if (server.minPort && tcp_port < server.minPort)
	{
		internal_fail("this server does not test ports below "
		              + std::to_string(server.minPort)
		              + ", and the peer port is "
		              + std::to_string(tcp_port));
		return;
	}

	state = State::Whoami;
	client->requestWhoami();
}

void QuickDC::DropCheck::EventDropWhoami(Drop::Client*, const Drop::Whoami& seen)
{
	if (state != State::Whoami) return;

	whoami = seen;

	result.ipv6 = (family == Drop::Family::IPv6);
	result.publicAddress = seen.ip;

	result.classification = seen.advice.empty()
		? Drop::toString(seen.classification) : seen.advice;

	result.port = tcp_port;

	state = State::Session;
	client->openSession();
}

void QuickDC::DropCheck::EventDropSession(Drop::Client*, const Drop::Session& opened)
{
	if (state != State::Session) return;

	session = opened;

	armed = true;

	std::vector<uint16_t> tcpPorts;
	std::vector<uint16_t> udpPorts;

	tcpPorts.push_back(tcp_port);
	if (want_udp) udpPorts.push_back(udp_port);

	state = State::Inbound;
	client->requestInbound(session, tcpPorts, udpPorts);
}

void QuickDC::DropCheck::EventDropInbound(Drop::Client*, const Drop::InboundResult& inbound)
{
	if (state != State::Inbound) return;

	const Drop::PortResult* tcp = inbound.findTcp(tcp_port);
	const Drop::PortResult* udp = want_udp ? inbound.findUdp(udp_port) : 0;

	const Drop::Decision decision = Drop::decide(family, whoami.classification,
	                                             tcp, udp, tcp_port);

	switch (decision.mode)
	{
		case Drop::Mode::Active:  result.mode = DropMode::Active; break;
		case Drop::Mode::Passive: result.mode = DropMode::Passive; break;
		default:                  result.mode = DropMode::Unknown; break;
	}

	result.tcpReached = reachOf(decision.tcp);
	result.udpReached = want_udp ? reachOf(decision.udp) : DropReach::Unknown;

	result.tcp = Drop::toString(decision.tcp);
	result.udp = want_udp ? Drop::toString(decision.udp) : std::string();
	result.reason = decision.reason;
	result.remedies = decision.remedies;
	result.note = decision.note;

	if (decision.advertisePort) result.port = decision.advertisePort;

	internal_finish();
}

void QuickDC::DropCheck::EventDropError(Drop::Client*, const Drop::ErrorInfo& info)
{
	if (!isBusy()) return;

	internal_fail(info.message());
}

const char* QuickDC::toString(QuickDC::DropMode mode)
{
	switch (mode)
	{
		case DropMode::Active:  return "active";
		case DropMode::Passive: return "passive";
		case DropMode::Unknown: return "not known";
	}

	return "not known";
}

/*
 * The three operations offered to whatever is outside this library; see
 * api/dropverdict.h for why they are free functions rather than methods.
 */

void QuickDC::setDropCheckListener(QuickDC::DropCheckListener* listener)
{
	Core* core = Core::getInstance();
	if (!core) return;

	if (!core->dropcheck) return;

	core->dropcheck->setObserver(listener);
}

bool QuickDC::startDropCheck(bool ipv6)
{
	Core* core = Core::getInstance();
	if (!core || !core->dropcheck || !core->config) return false;

	if (core->dropcheck->isBusy()) return false;

	core->config->setGroup("Network");

	const std::string server = core->config->getString("Reachability server", "");
	if (server.empty()) return false;

	const uint16_t port = static_cast<uint16_t>(core->config->getNumber("Port", 1411));

	/*
	 * The family's own name, never the unprefixed one: a connection has one
	 * source address, so the dual-stack name would measure whichever family that
	 * connection happened to use rather than the one being asked about.
	 */
	const std::string host = (ipv6 ? "https://ipv6." : "https://ipv4.") + server;

	core->dropcheck->start(host,
	                       ipv6 ? Drop::Family::IPv6 : Drop::Family::IPv4,
	                       port, port,
	                       std::optional<Samurai::IO::Net::InetAddress>());
	return true;
}

QuickDC::DropVerdict QuickDC::lastDropVerdict(bool ipv6)
{
	Core* core = Core::getInstance();
	if (!core || !core->dropcheck) return DropVerdict();

	return core->dropcheck->verdict(ipv6);
}


bool QuickDC::dropMeasurementIsFresh(const QuickDC::DropVerdict& held, time_t now)
{
	if (!held.ran || !held.observedAt) return false;

	if (now < held.observedAt) return false;

	return now - held.observedAt <= QUICKDC_REACHABILITY_MEASUREMENT_AGE;
}

void QuickDC::applyDropMeasurement()
{
	Core* core = Core::getInstance();
	if (!core) return;

	/* Each family's own, into the record that answers for it and no other. */
	for (const bool ipv6 : {false, true})
	{
		const DropVerdict verdict = lastDropVerdict(ipv6);
		if (!verdict.ran) continue;

		Reachability* record = core->reachabilityFor(ipv6);
		if (!record) continue;

		record->measured(stateOf(verdict.tcpReached),
		                 stateOf(verdict.udpReached),
		                 verdict.observedAt);
	}
}
