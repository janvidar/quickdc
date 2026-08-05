/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/reachability.h"
#include "debug/dbg.h"

const char* QuickDC::Reachability::toString(State state)
{
	switch (state)
	{
		case State::Reachable:   return "reachable";
		case State::Unreachable: return "unreachable";
		default:                 return "not known";
	}
}


void QuickDC::Reachability::advertising(const std::string& address_, uint16_t port_)
{
	if (advertised && address == address_ && port == port_) return;

	if (advertised)
		QDBG("The endpoint changed from %s:%d to %s:%d; what was known about the "
			"old one says nothing about the new one",
			address.c_str(), static_cast<int>(port),
			address_.c_str(), static_cast<int>(port_));

	forget();

	address = address_;
	port = port_;
	advertised = true;
}


void QuickDC::Reachability::inboundConnection()
{
	if (inbound_connection) return;

	QDBG("A peer connected to us from outside; the peer port is reachable");
	inbound_connection = true;
}


void QuickDC::Reachability::inboundDatagram()
{
	if (inbound_datagram) return;

	QDBG("A datagram we asked for arrived; the peer port is reachable over UDP");
	inbound_datagram = true;
}


void QuickDC::Reachability::connectionRefused(const std::string& peer)
{
	if (peer.empty()) return;

	/*
	 * Stopped at the threshold rather than kept for ever: reports past it change
	 * nothing, and the set is fed by whoever is on the hub.
	 */
	if (refused_by.size() >= QUICKDC_REACHABILITY_MAX_REPORTERS) return;

	if (!refused_by.insert(peer).second) return;

	QDBG("'%s' could not connect to us (%zu of %d needed to stop claiming we are "
		"reachable)", peer.c_str(), refused_by.size(), QUICKDC_REACHABILITY_REFUSALS);
}


void QuickDC::Reachability::connectionCheckFailed()
{
	if (!advertised) return;

	QDBG("A hub could not connect to %s:%d", address.c_str(), (int) port);
	check_failed = true;
}


QuickDC::Reachability::State QuickDC::Reachability::tcp() const
{
	/*
	 * Something that arrived settles it, whatever anyone says afterwards. A
	 * refusal is a peer's account of its own failure and can be mistaken or
	 * malicious; a connection that arrived is not an account of anything.
	 */
	if (inbound_connection) return State::Reachable;

	/* One hub is enough, where three peers are needed. See the header for why
	   these two are not the same kind of report. */
	if (check_failed) return State::Unreachable;

	if (refused_by.size() >= QUICKDC_REACHABILITY_REFUSALS)
		return State::Unreachable;

	return State::Unknown;
}


QuickDC::Reachability::State QuickDC::Reachability::udp() const
{
	/*
	 * Only ever positive. A peer that cannot reach our UDP port has nothing to
	 * report it with - there is no datagram it could send to say so, which is the
	 * whole difficulty - so silence is the only symptom and silence is not proof.
	 */
	if (inbound_datagram) return State::Reachable;

	return State::Unknown;
}


void QuickDC::Reachability::forget()
{
	inbound_connection = false;
	inbound_datagram = false;
	check_failed = false;
	refused_by.clear();
}
