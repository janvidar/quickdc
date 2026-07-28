/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include <samurai/io/buffer.h>
#include <samurai/io/net/datagram.h>
#include <stdlib.h>

#include "api/core.h"
#include "api/hub.h"
#include "network/adc/adcudp.h"
#include "network/adc/parser.h"
#include "network/hubmanager.h"
#include "network/downloadqueue.h"
#include "network/hubsession.h"
#include "network/usermanager.h"
#include "share/searchresult.h"

void ADC::DatagramHandler::onDatagram(Samurai::IO::Net::DatagramSocket*, Samurai::IO::Net::DatagramPacket* packet)
{
	if (!packet || !packet->getBuffer()) return;

	size_t length = packet->getBuffer()->size();
	if (!length) return;

	char* raw = packet->getBuffer()->memdup(0, length);
	if (!raw) return;

	/* TCP framing strips the terminator before parsing; nothing frames a
	   datagram, so trim it or the last argument keeps it as part of its value. */
	while (length && (raw[length - 1] == '\n' || raw[length - 1] == '\r'))
		raw[--length] = '\0';

	if (!length)
	{
		free(raw);
		return;
	}

	ADC::Command cmd;
	if (ADC::Parser::parse(raw, length, cmd))
	{
		switch (cmd.getFourcc())
		{
			case FOURCC('U','R','E','S'):
				onSearchResult(&cmd);
				break;

			case FOURCC('U','P','S','R'):
				onPartialStatus(&cmd);
				break;

			default:
				QDBG("Ignoring an ADC datagram we have no use for");
				break;
		}
	}
	else
	{
		QDBG("Unparseable ADC datagram, %d bytes", (int) length);
	}

	free(raw);
}

void ADC::DatagramHandler::onPartialStatus(ADC::Command* cmd)
{
	char* tth  = cmd->getArgument("TR", 2, 0);
	char* pi   = cmd->getArgument("PI", 2, 0);
	char* nick = cmd->getArgument("NI", 2, 0);
	const char* cid = cmd->getSourceID();

	if (!tth || !pi) return;

	QuickDC::Core* core = QuickDC::Core::getInstance();
	if (!core || !core->hubs || !core->queue) return;

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

void ADC::DatagramHandler::onSearchResult(ADC::Command* cmd)
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

	QuickDC::Core* core = QuickDC::Core::getInstance();
	if (!core || !core->hubs) return;

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

	uint64_t size = (s_size) ? samurai_atoull(s_size) : 0;
	size_t depth  = (s_depth) ? (size_t) samurai_atoi(s_depth) : 0;
	size_t slots  = (s_slots) ? (size_t) samurai_atoi(s_slots) : 0;

	QuickDC::Share::SearchResult* result =
		new QuickDC::Share::SearchResult(filename, size, slots, tth, depth, token);

	/* Borrowed from the user manager; the cast is because SearchResult stores
	   a mutable pointer. */
	result->setUser(const_cast<QuickDC::User*>(users->getUserByClientID(cid)));

	/* Takes ownership of 'result'. */
	hub->onSearchResult(result);
}
