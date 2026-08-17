/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/dropcheck.h"
#include "api/core.h"
#include "config/preferences.h"

#include <drop/responder.h>
#include <drop/stun.h>

#include <samurai/io/net/inetaddress.h>

#include <time.h>

QuickDC::DropCheck::DropCheck(DropCheckListener* listener_) : listener(listener_)
{
}

QuickDC::DropCheck::~DropCheck()
{
	/*
	 * Not through cancel(), which reports nothing but leaves the client in place:
	 * here the client goes, and the order matters only in that the credentials
	 * stop answering before the socket that carried them can be read again.
	 */
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

	if (!tcpPort)
	{
		internal_fail("there is no peer port to test");
		return;
	}

	family = family_;
	tcp_port = tcpPort;
	udp_port = udpPort;
	want_udp = false;

	server = Drop::ServerInfo();
	whoami = Drop::Whoami();
	internal_disarm();

	Drop::Client::Options options;

	/*
	 * Pinned where the caller knew which address to pin. A host with several
	 * addresses of one family has no source address until one is named - the
	 * kernel picks per destination - so without this the server would report,
	 * and probe, whichever address it happened to be reached from rather than
	 * the one this client advertises.
	 */
	if (localAddress) options.localAddress = *localAddress;

	options.userAgent = PRODUCT "/" VERSION;

	/*
	 * Replaced rather than reset first: the previous run's client owns the socket
	 * whose callback may be what is running now, and this is reached from a
	 * handler whenever a check is asked for by one.
	 */
	client = std::make_unique<Drop::Client>(this, baseUrl, family, options);

	/*
	 * Capabilities first, which is also what says this is a DROP server rather
	 * than something that answers 200 to everything - and what carries the
	 * bounds the rest of the run has to stay inside.
	 */
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
}

void QuickDC::DropCheck::internal_finish()
{
	state = State::Idle;

	/*
	 * Before the verdict is reported rather than after, so that nothing can
	 * observe a client still willing to answer a probe for a run that is over.
	 */
	internal_disarm();

	result.observedAt = time(0);
	result.ran = true;

	QDBG("Reachability: %s on %s port %d, seen at %s",
	     toString(result.mode), result.ipv6 ? "IPv6" : "IPv4",
	     static_cast<int>(result.port), result.publicAddress.c_str());

	if (listener) listener->EventDropCheckFinished(result);
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
	/*
	 * The empty credentials of a run that is not open are themselves the refusal
	 * - handleStunProbe() answers nothing without a password - but the state is
	 * checked here as well, so that the rule is visible where it is relied upon
	 * rather than only inside the library.
	 */
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

	/*
	 * Asked for only where the server offers it for this family. The two lists
	 * routinely differ - the second address RFC 5780 needs is commonly there on
	 * one family and not the other - and asking for a test a server does not run
	 * is a refusal rather than an answer.
	 */
	want_udp = udp_port && server.supports(family, "udp");

	/*
	 * A port the server will not probe cannot be measured, and saying so is
	 * better than reporting the refusal that would follow: the bound is the
	 * server's, so nothing this client does about its own configuration would
	 * make the answer arrive.
	 */
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

	/* The server's own wording where it gave one, its bare classification when
	   it did not - either way what a user is shown is what the measurer said. */
	result.classification = seen.advice.empty()
		? Drop::toString(seen.classification) : seen.advice;

	result.port = tcp_port;

	/*
	 * Nothing is armed here, because the listeners this asks about are the ones
	 * already up: the peer port and the search port are bound for as long as the
	 * client runs. What a session adds is the credentials that let a probe be
	 * told apart from a stranger, and those arrive with the session below.
	 */
	state = State::Session;
	client->openSession();
}

void QuickDC::DropCheck::EventDropSession(Drop::Client*, const Drop::Session& opened)
{
	if (state != State::Session) return;

	session = opened;

	/*
	 * Armed before the probes are asked for and not after. The server connects
	 * back while the request is outstanding, so a client that asks first races
	 * the probe against its own willingness to answer it.
	 */
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

	/*
	 * The library's own table, rather than a reading of the verdicts here. It
	 * lives in one place on purpose: a user who moves between two clients and is
	 * given two different explanations of the same network concludes that one of
	 * them is broken.
	 */
	const Drop::Decision decision = Drop::decide(family, whoami.classification,
	                                             tcp, udp, tcp_port);

	switch (decision.mode)
	{
		case Drop::Mode::Active:  result.mode = DropMode::Active; break;
		case Drop::Mode::Passive: result.mode = DropMode::Passive; break;
		default:                  result.mode = DropMode::Unknown; break;
	}

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
	/*
	 * Reached from inside a request as well as from the wire: a request refused
	 * before it reaches a socket reports itself from the call that made it, which
	 * is why every state is set before its request rather than after.
	 */
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

	/*
	 * Made on demand where there is none, so that a server named after this
	 * started is usable without a restart - the check is only absent because
	 * nothing had been configured to ask.
	 */
	if (!core->dropcheck)
	{
		if (!listener) return;
		core->dropcheck = new DropCheck(listener);
		return;
	}

	/*
	 * A run in flight belongs to whoever is being displaced. It cannot simply be
	 * left going: the connect-back is recognised through whichever check this
	 * names - the protocol probe asks it - so a run whose listener has changed
	 * would answer the challenge and report to somebody who is no longer there,
	 * and one whose listener is gone would conclude passive on no evidence.
	 */
	if (core->dropcheck->isBusy()) core->dropcheck->cancel();

	core->dropcheck->setListener(listener);
}

bool QuickDC::startDropCheck()
{
	Core* core = Core::getInstance();
	if (!core || !core->dropcheck || !core->config) return false;

	/* A run already going will report on its own, and asking twice would only
	   spend a session the server counts against us. */
	if (core->dropcheck->isBusy()) return false;

	core->config->setGroup("Network");

	const std::string server = core->config->getString("Reachability server", "");
	if (server.empty()) return false;

	const uint16_t port = static_cast<uint16_t>(core->config->getNumber("Port", 1411));

	/*
	 * The family's own name. One connection has one source address, so the
	 * dual-stack name would measure whichever family that connection used rather
	 * than the one being asked about.
	 */
	core->dropcheck->start("https://ipv4." + server, Drop::Family::IPv4, port, port,
	                       std::optional<Samurai::IO::Net::InetAddress>());
	return true;
}

QuickDC::DropVerdict QuickDC::lastDropVerdict()
{
	Core* core = Core::getInstance();
	if (!core || !core->dropcheck) return DropVerdict();

	return core->dropcheck->verdict();
}
