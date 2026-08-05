/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/adc/server/claimcheck.h"

#include "debug/dbg.h"
#include "network/adc/adccommon.h"
#include "network/adc/parser.h"
#include "network/adc/server/adchub.h"
#include "network/adc/server/adchubuser.h"

#include <samurai/io/net/socket.h>

#include <memory>
#include <vector>

namespace {

/*
 * The checks this hub has made, running and finished both.
 *
 * Held here rather than by the hub for two reasons. A check has to outlive the event
 * that ended it - it is told the connection failed by the socket it owns, so it cannot
 * free itself while that call is still on the stack - and a finished one is the record
 * the interval is read from, so it has something to say after it has stopped.
 *
 * Swept when the next check starts, which is the only thing that adds to the list.
 */
std::vector<std::unique_ptr<ADC::ClaimCheck>> checks;

}


size_t ADC::ClaimCheck::running()
{
	size_t live = 0;
	for (const auto& check : checks)
		if (check && !check->finished) live++;
	return live;
}


void ADC::ClaimCheck::forget(Hub* hub)
{
	for (size_t n = checks.size(); n > 0; n--)
	{
		if (checks[n - 1] && checks[n - 1]->hub == hub)
			checks.erase(checks.begin() + static_cast<long>(n - 1));
	}
}


void ADC::ClaimCheck::reset()
{
	checks.clear();
}


ADC::ClaimCheck* ADC::ClaimCheck::start(Hub* hub, HubUser* user,
                                        const char* protocol, uint16_t port)
{
	if (!hub || !user) return 0;

	/*
	 * A privileged port is not where a peer transfer listens, and it is where
	 * everything else does. The port is the one thing here the client chose, and the
	 * address it goes with is shared with whoever else is behind the same NAT, so the
	 * rule that matters is to refuse the ports that are interesting to reach.
	 */
	if (port < 1024)
	{
		QDBG("Not checking port %d: not a port a peer connection listens on",
			static_cast<int>(port));
		return 0;
	}

	/*
	 * The address the hub observed, never one the client named. That is the whole of
	 * what keeps this from being a way to have a hub connect somewhere on request.
	 */
	const std::string address = user->getObservedAddress();
	if (address.empty())
	{
		QDBG("Not checking port %d: the connection has no address to check",
			static_cast<int>(port));
		return 0;
	}

	const std::chrono::steady_clock::time_point now =
		std::chrono::steady_clock::now();
	const std::chrono::seconds interval(QUICKDC_CLAIMCHECK_INTERVAL_SECONDS);

	/* Swept here because this is the only thing that grows the list. */
	for (size_t n = checks.size(); n > 0; n--)
	{
		const std::unique_ptr<ClaimCheck>& check = checks[n - 1];
		const bool stale = check && check->finished
			&& now - *check->finished >= interval;
		if (!check || stale)
			checks.erase(checks.begin() + static_cast<long>(n - 1));
	}

	/*
	 * One check per endpoint per interval, which is what stops a client that asks for
	 * a connection every few seconds from being checked on every one of them. Keyed
	 * on the endpoint rather than on the session, so that reconnecting - which draws a
	 * new SID - does not clear the record of what was just checked.
	 */
	for (const auto& check : checks)
	{
		if (check->address == address && check->port == port)
		{
			QDBG("Not checking %s:%d: %s", address.c_str(), static_cast<int>(port),
				check->finished ? "checked recently" : "already being checked");
			return 0;
		}
	}

	if (running() >= QUICKDC_CLAIMCHECK_CONCURRENT)
	{
		QDBG("Not checking %s:%d: %d checks are already running", address.c_str(),
			static_cast<int>(port), QUICKDC_CLAIMCHECK_CONCURRENT);
		return 0;
	}

	/*
	 * The oldest record goes when there are too many, which loses an interval rather
	 * than a check: what it costs is that one endpoint may be checked again sooner
	 * than the interval says. Running checks are never dropped - one of those owns a
	 * socket that would report to freed memory.
	 */
	while (checks.size() >= QUICKDC_CLAIMCHECK_REMEMBERED)
	{
		auto oldest = checks.end();
		for (auto at = checks.begin(); at != checks.end(); ++at)
		{
			if (!(*at)->finished) continue;
			if (oldest == checks.end() || *(*at)->finished < *(*oldest)->finished)
				oldest = at;
		}
		if (oldest == checks.end()) break;
		checks.erase(oldest);
	}

	ClaimCheck* check = new ClaimCheck(hub, user->getSID(), address, port,
		protocol ? protocol : "");
	checks.emplace_back(check);
	return check;
}


ADC::ClaimCheck::ClaimCheck(Hub* hub_, sid_t sid_, const std::string& address_,
                            uint16_t port_, const std::string& protocol_)
	: hub(hub_), sid(sid_), address(address_), port(port_), protocol(protocol_)
{
	QDBG("Checking whether %s:%d can be reached", address.c_str(),
		static_cast<int>(port));

	socket = Samurai::IO::Net::Socket::create(this, address, port);
	if (!socket)
	{
		finished = std::chrono::steady_clock::now();
		return;
	}

	/*
	 * A shorter deadline than an ordinary connection's, because what is being asked
	 * is narrower: a peer transfer may wait a while for a busy client, and this only
	 * wants to know whether anything is listening at all.
	 */
	socket->setConnectTimeout(
		std::chrono::seconds(QUICKDC_CLAIMCHECK_TIMEOUT_SECONDS));

	socket->connect();
}


ADC::ClaimCheck::~ClaimCheck()
{
	/*
	 * A running check being destroyed - the hub it reports to is going away - still
	 * owns a socket whose handler is this object. Released the same way finish()
	 * releases it, because the socket monitor outlives both.
	 */
	if (socket)
	{
		socket->setEventHandler(0);
		socket->disconnect();
	}
}


void ADC::ClaimCheck::EventConnected(const Samurai::IO::Net::Socket*)
{
	reachable();
}


void ADC::ClaimCheck::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	/*
	 * A close after the connection was up, which happens either way: a client that
	 * accepted it may drop it again once it sees nothing coming. Arriving here without
	 * having connected is the failure, and finish() is what tells the two apart.
	 */
	if (!finished) unreachable("the connection was closed before it was established");
}


void ADC::ClaimCheck::EventError(const Samurai::IO::Net::Socket*,
                                 Samurai::IO::Net::SocketError, const char* msg)
{
	unreachable(msg && *msg ? msg : "the connection failed");
}


void ADC::ClaimCheck::reachable()
{
	if (finished) return;
	finish();

	/*
	 * Nothing is said to the client. The claim was true, and a hub telling a client
	 * that its settings are correct is a message nobody needs.
	 */
	QDBG("%s:%d answered; the claim holds", address.c_str(), static_cast<int>(port));
}


/* 'why' is the log's business only: see ADC_STR_CONNECTION_FAILED for why a
   system's error text is not sent to the client it is about. */
void ADC::ClaimCheck::unreachable([[maybe_unused]] const char* why)
{
	if (finished) return;
	finish();

	QDBG("%s:%d could not be reached: %s", address.c_str(), static_cast<int>(port), why);

	/*
	 * Looked up now rather than remembered, because the client may have gone in the
	 * seconds this took - and one that has gone is not one to tell anything.
	 */
	HubUser* user = hub->lookupUser(sid);
	if (!user) return;

	/*
	 * Severity 1, recoverable, and it is: the client is still connected, still able
	 * to download, and what has to change is its own configuration.
	 *
	 * ADC_CODE_PROTOCOL_CONNECT_ERROR is the code a peer sends when a connection it
	 * was asked to make did not succeed, which is exactly what happened - the hub is
	 * reporting it about itself rather than relaying it for someone else. It carries
	 * 'PR', so the port is named in a field and not only in the text.
	 */
	ADC::Command* cmd = new ADC::Command(FOURCC('I','S','T','A'));
	cmd->addArgument(adc_status_code(ADC_STATUS_SEVERITY_ERROR,
		ADC_CODE_PROTOCOL_CONNECT_ERROR).c_str());
	const std::string what = "Nothing answered on port " + std::to_string(port)
		+ ", so other clients cannot connect to you";
	cmd->addArgument(what.c_str());

	/*
	 * 'PR' is the protocol, which is what the field means - not the port. There is no
	 * base ADC field for a port, and inventing one here would put a private dialect
	 * on the wire for the sake of a number the client already knows: it named this
	 * port itself, in the CTM this is the answer to.
	 *
	 * What makes this machine readable without a new field is where it came from. A
	 * status in the hub's own context, carrying no 'TO', is the hub reporting about
	 * the connection it made - not relaying another client's failure.
	 */
	if (!protocol.empty()) cmd->addArgument("PR", protocol);

	hub->send(cmd, user);
}


void ADC::ClaimCheck::finish()
{
	finished = std::chrono::steady_clock::now();

	/*
	 * The socket is released rather than deleted, and this object stays behind: the
	 * event that got here came from the socket, so tearing it down inside its own
	 * handler would return into freed memory. start() sweeps what has gone stale.
	 */
	if (socket)
	{
		socket->setEventHandler(0);
		socket->disconnect();
		socket.reset();
	}
}
