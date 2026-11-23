/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <samurai/io/buffer.h>
#include <samurai/io/net/datagram.h>
#include <string>

#include "api/core.h"
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socketaddress.h>
#include "network/searchtokens.h"
#include "network/reachability.h"
#include "api/hub.h"
#include "network/adc/adcudp.h"
#include "network/adc/parser.h"
#include "network/hubmanager.h"
#include "network/downloadqueue.h"
#include "network/hubsession.h"
#include "network/usermanager.h"
#include "share/searchresult.h"

void ADC::DatagramHandler::onDatagram(QuickDC::Core& core, Samurai::IO::Net::DatagramSocket*, Samurai::IO::Net::DatagramPacket* packet)
{
	if (!packet || !packet->getBuffer()) return;

	const size_t length = packet->getBuffer()->size();
	if (!length) return;

	std::string raw = packet->getBuffer()->copyRange(0, length);

	/* TCP framing strips the terminator before parsing; nothing frames a
	   datagram, so trim it or the last argument keeps it as part of its value. */
	while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r'))
		raw.pop_back();

	if (raw.empty()) return;

	ADC::Command cmd;
	if (ADC::Parser::parse(raw.c_str(), raw.size(), cmd))
	{
		switch (cmd.getFourcc())
		{
			case FOURCC('U','R','E','S'):
				noteReachable(core, packet, &cmd);
				onSearchResult(core, &cmd);
				break;

			case FOURCC('U','P','S','R'):
				onPartialStatus(core, &cmd);
				break;

			default:
				QDBG("Ignoring an ADC datagram we have no use for");
				break;
		}
	}
	else
	{
		QDBG("Unparseable ADC datagram, %d bytes", static_cast<int>(raw.size()));
	}
}

void ADC::DatagramHandler::onPartialStatus(QuickDC::Core& core_, ADC::Command* cmd)
{
	char* tth  = cmd->getArgument("TR", 2, 0);
	char* pi   = cmd->getArgument("PI", 2, 0);
	char* nick = cmd->getArgument("NI", 2, 0);
	const char* cid = cmd->getSourceID();

	if (!tth || !pi) return;

	QuickDC::Core* core = &core_;
	if (!core->hubs || !core->queue) return;

	/* On an NMDC hub we will not know the sender's client ID; NI is what
	   identifies them, and is how a FileSource records an NMDC user. */
	QuickDC::Hub* hub = (cid) ? core->hubs->lookupUserByClientID(cid) : 0;
	const char* id = cid;

	if (!hub && nick)
	{
		hub = core->hubs->lookupUserByNick(nick);
		id = nick;
	}

	if (!hub || !id)
	{
		QDBG("Ignoring partial file status from a peer we share no hub with");
		return;
	}

	core->queue->onPartialSources(hub->getUrl(), id, tth, pi);
}

/*
 * A datagram arriving on the peer port is the only thing that shows the port can be
 * reached over UDP - no peer that fails to reach it has any way of saying so, which
 * is why nothing else can establish it.
 *
 * Two things are required of it. It has to answer a search this client sent, because
 * a source address is whatever its sender wrote and an unsolicited datagram
 * therefore shows nothing; and it must not have come from this machine, because the
 * loopback shows only that the socket is open, which was never the question.
 *
 * What the token does not establish is who sent it: a search goes to every client on
 * the hub, so its token is public from the moment it is sent. It bounds this to "an
 * answer to something we asked" rather than "from the peer we expected", which is as
 * much as a datagram can be held to.
 */
void ADC::DatagramHandler::noteReachable(QuickDC::Core& core_, Samurai::IO::Net::DatagramPacket* packet,
                                         ADC::Command* cmd)
{
	QuickDC::Core* core = &core_;
	if (!core->reachability || !core->searchTokens) return;

	const char* token = cmd->getArgument("TO", 2, 0);
	if (!core->searchTokens->issued(token))
	{
		QDBG("A datagram answering no search of ours; not evidence of anything");
		return;
	}

	if (!packet) return;

	/*
	 * The address is only reachable through the internet flavour of it, and a
	 * datagram read from the socket has one - but a caller could hand this
	 * something else, so it is asked rather than assumed.
	 */
	const Samurai::IO::Net::InetSocketAddress* from =
		dynamic_cast<const Samurai::IO::Net::InetSocketAddress*>(packet->getAddress());
	if (!from) return;

	const Samurai::IO::Net::InetAddress* address = from->getAddress();
	if (!address || address->isLoopback()) return;

	/* Against the family it arrived on; see DCServer::EventAcceptSocket(). */
	const bool six =
		address->getType() == Samurai::IO::Net::InetAddress::Version::IPv6;

	if (QuickDC::Reachability* record = core->reachabilityFor(six))
		record->inboundDatagram();
}


void ADC::DatagramHandler::onSearchResult(QuickDC::Core& core_, ADC::Command* cmd)
{
	/* The argument names mirror what HubSession::EventSearchReply() sends. */
	char* filename = cmd->getArgument("FN", 2, 0);
	char* s_size   = cmd->getArgument("SI", 2, 0);
	char* tth      = cmd->getArgument("TR", 2, 0);
	char* s_depth  = cmd->getArgument("TD", 2, 0);
	char* token    = cmd->getArgument("TO", 2, 0);
	char* s_slots  = cmd->getArgument("SL", 2, 0);

	if (!filename || !*filename)
	{
		QDBG("Ignoring a UDP search result with no filename");
		return;
	}

	/* A datagram names its sender by CID; there is no session to resolve a
	   SID against. */
	const char* cid = cmd->getSourceID();
	if (!cid)
	{
		QDBG("Ignoring a UDP search result from nobody");
		return;
	}

	QuickDC::Core* core = &core_;
	if (!core->hubs) return;

	/* Nothing says which hub the search went out on, and the CID is globally
	   unique, so any hub the sender is on will reach them. */
	QuickDC::Hub* hub = core->hubs->lookupUserByClientID(cid);
	if (!hub)
	{
		QDBG("Ignoring a UDP search result from '%s', who is not on any hub we are on", cid);
		return;
	}

	QuickDC::UserManager* users = hub->getUserManager();
	if (!users) return;

	uint64_t size = (s_size) ? Samurai::Util::Convert::to_uint64(s_size) : 0;
	size_t depth  = (s_depth) ? static_cast<size_t>(Samurai::Util::Convert::to_int32(s_depth)) : 0;
	size_t slots  = (s_slots) ? static_cast<size_t>(Samurai::Util::Convert::to_int32(s_slots)) : 0;

	QuickDC::Share::SearchResult* result =
		new QuickDC::Share::SearchResult(filename, size, slots, tth, depth, token);

	/* Borrowed from the user manager; the cast is because SearchResult stores
	   a mutable pointer. */
	result->setUser(const_cast<QuickDC::User*>(users->getUserByClientID(cid)));

	/* Takes ownership of 'result'. */
	hub->onSearchResult(result);
}
