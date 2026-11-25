/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dropcheck.h"
#include "api/core.h"
#include "config/preferences.h"
#include "network/reachability.h"
#include "config/settings.h"

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

QuickDC::DropCheck::DropCheck(Samurai::EventLoop& loop_, DropCheckListener* listener_)
	: loop(loop_), listener(listener_)
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
	address_only = false;

	internal_begin(baseUrl, localAddress);
}

void QuickDC::DropCheck::startAddressLookup(const std::string& baseUrl, Drop::Family family_,
                                            const std::optional<Samurai::IO::Net::InetAddress>& localAddress)
{
	if (isBusy())
	{
		internal_fail("a reachability check is already running");
		return;
	}

	family = family_;

	/*
	 * No ports at all, and nothing left over from an earlier run either: the
	 * verdict this leaves names the port it tested, and a lookup tested none.
	 */
	tcp_port = 0;
	udp_port = 0;
	want_udp = false;
	address_only = true;

	internal_begin(baseUrl, localAddress);
}

void QuickDC::DropCheck::internal_begin(const std::string& baseUrl,
                                        const std::optional<Samurai::IO::Net::InetAddress>& localAddress)
{
	server = Drop::ServerInfo();
	whoami = Drop::Whoami();
	internal_disarm();

	/*
	 * The verdict starts empty, so that a run says only what it established. A
	 * lookup leaves the reachability fields alone by never reaching them, and
	 * without this it would report whatever the last measurement had put there.
	 */
	result = DropVerdict();
	result.addressOnly = address_only;

	Drop::Client::Options options;

	if (localAddress) options.localAddress = *localAddress;

	options.userAgent = PRODUCT "/" VERSION;

	client = std::make_unique<Drop::Client>(loop, this, baseUrl, family, options);

	state = State::ServerInfo;

	/*
	 * Announced before the first request goes out, and after the state says a run
	 * is going: a listener told about it is entitled to ask what is happening, and
	 * one told afterwards would be told about a run that had already answered on a
	 * server that responds fast enough.
	 */
	const bool ipv6 = (family == Drop::Family::IPv6);

	if (listener) listener->EventDropCheckStarted(ipv6);
	observers.notify([ipv6](DropCheckListener& l) { l.EventDropCheckStarted(ipv6); });

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

	/*
	 * Which family failed, because one run answers for one of them and the two
	 * are commonly two different servers: a reader told only that the check
	 * failed cannot tell a host with no IPv6 route - where nothing is wrong and
	 * nothing is owed - from a server that is down for both.
	 */
	const std::string said =
		std::string(family == Drop::Family::IPv6 ? "IPv6" : "IPv4") + ": " + reason;

	if (listener) listener->EventDropCheckFailed(said.c_str());
	observers.notify([&said](DropCheckListener& l) { l.EventDropCheckFailed(said.c_str()); });
}

void QuickDC::DropCheck::internal_finish()
{
	state = State::Idle;
	internal_disarm();

	result.observedAt = time(0);
	result.ran = true;

	/*
	 * Said differently for a lookup, because the words the measurement uses are
	 * about a port and a mode and a lookup established neither: a line reading
	 * "not known on port 0" describes a failed measurement, which this is not.
	 */
	if (result.addressOnly)
		QDBG("The public %s address is %s (%s)",
		     result.ipv6 ? "IPv6" : "IPv4", result.publicAddress.c_str(),
		     result.classification.c_str());
	else
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
	observers.notify([&reported](DropCheckListener& l) { l.EventDropCheckFinished(reported); });
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

	/*
	 * The bound is on ports the server will probe, so it says nothing about a run
	 * that asks it to probe none. Applied to a lookup it would refuse every server
	 * that names one, over the port a lookup does not have.
	 */
	if (!address_only && server.minPort && tcp_port < server.minPort)
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

	/*
	 * A lookup is over here, with the address it came for and nothing else. The
	 * session is what would arm the peer port and the search port to answer a
	 * stranger, so a run that wants no probe must never open one: there is nothing
	 * for it to hold credentials against, and the window would be open for as long
	 * as the run took anyway.
	 */
	if (address_only)
	{
		internal_finish();
		return;
	}

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

	std::string reason = info.message();

	/*
	 * A family-specific name that could not be reached at all, before anything
	 * had been established, is most often this host having no connectivity in
	 * that family rather than anything to fix: the check is pointed at the
	 * ipv4. or ipv6. name precisely so that one family is measured, and a host
	 * with no route of one resolves its name to nothing. Saying so is the
	 * difference between a failure to act on and one to ignore.
	 */
	if (info.error == Drop::Error::Transport && state == State::ServerInfo)
	{
		reason += "; this host may have no ";
		reason += family == Drop::Family::IPv6 ? "IPv6" : "IPv4";
		reason += " connectivity, in which case there is nothing to test and "
		          "nothing wrong";
	}

	internal_fail(reason);
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

void QuickDC::setDropCheckListener(QuickDC::Core& core, QuickDC::DropCheckListener* listener)
{
	if (!core.dropcheck) return;

	core.dropcheck->setObserver(listener);
}

QuickDC::Subscription QuickDC::observeDropCheck(QuickDC::Core& core, QuickDC::DropCheckListener* listener)
{
	if (!core.dropcheck) return Subscription();

	return core.dropcheck->observe(listener);
}

namespace {

/**
 * The server to ask about 'ipv6', or nothing where there is nobody to ask.
 *
 * The family's own name, never the unprefixed one: a connection has one source
 * address, so the dual-stack name would answer for whichever family that
 * connection happened to use rather than the one being asked about. Shared by both
 * entry points below, so that a lookup and a measurement cannot come to ask two
 * different servers about the same family.
 */
std::string dropServerFor(QuickDC::Core* core, bool ipv6)
{
	if (!core->config) return std::string();

	const std::string server = QuickDC::Settings::read(*core->config, QuickDC::Settings::ReachabilityServer);
	if (server.empty()) return std::string();

	return (ipv6 ? "https://ipv6." : "https://ipv4.") + server;
}

}

bool QuickDC::startDropCheck(Core& core_, bool ipv6)
{
	Core* core = &core_;
	if (!core->dropcheck || !core->config) return false;

	if (core->dropcheck->isBusy()) return false;

	const std::string host = dropServerFor(core, ipv6);
	if (host.empty()) return false;

	const uint16_t port = core->peerPort();

	core->dropcheck->start(host,
	                       ipv6 ? Drop::Family::IPv6 : Drop::Family::IPv4,
	                       port, port,
	                       std::optional<Samurai::IO::Net::InetAddress>());
	return true;
}

bool QuickDC::startAddressLookup(Core& core_, bool ipv6)
{
	Core* core = &core_;
	if (!core->dropcheck) return false;

	/*
	 * A measurement in flight is not displaced for this. The two share the one
	 * client, and the run that probes a port is the one worth waiting for - it
	 * reports the address as well.
	 */
	if (core->dropcheck->isBusy()) return false;

	const std::string host = dropServerFor(core, ipv6);
	if (host.empty()) return false;

	core->dropcheck->startAddressLookup(host,
	                                    ipv6 ? Drop::Family::IPv6 : Drop::Family::IPv4,
	                                    std::optional<Samurai::IO::Net::InetAddress>());
	return true;
}

QuickDC::DropVerdict QuickDC::lastDropVerdict(const Core& core_, bool ipv6)
{
	const Core* core = &core_;
	if (!core->dropcheck) return DropVerdict();

	return core->dropcheck->verdict(ipv6);
}


bool QuickDC::dropMeasurementIsFresh(const QuickDC::DropVerdict& held, time_t now)
{
	if (!held.ran || !held.observedAt) return false;

	/*
	 * A lookup is not a measurement, however recently it was taken: nothing
	 * connected back for it, so there is nothing in it to be fresh. Answering yes
	 * would let an address looked up when a settings page opened stand in for the
	 * run this question is asked in order to schedule, and suppress it for the
	 * whole of QUICKDC_REACHABILITY_MEASUREMENT_AGE.
	 */
	if (held.addressOnly) return false;

	if (now < held.observedAt) return false;

	return now - held.observedAt <= QUICKDC_REACHABILITY_MEASUREMENT_AGE;
}

void QuickDC::applyDropMeasurement(Core& core_)
{
	Core* core = &core_;

	/* Each family's own, into the record that answers for it and no other. */
	for (const bool ipv6 : {false, true})
	{
		const DropVerdict verdict = lastDropVerdict(core_, ipv6);
		if (!verdict.ran) continue;

		/*
		 * A lookup probed nothing, so both of its reach values are Unknown, and
		 * Unknown is written into the record like any other answer. Offering one
		 * here would take away what the last real measurement established, on the
		 * strength of a run that never asked the question.
		 */
		if (verdict.addressOnly) continue;

		Reachability* record = core->reachabilityFor(ipv6);
		if (!record) continue;

		record->measured(stateOf(verdict.tcpReached),
		                 stateOf(verdict.udpReached),
		                 verdict.observedAt);
	}
}
