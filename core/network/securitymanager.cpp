/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/securitymanager.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/util/random.h>
#include <stdlib.h>
#include <time.h>

/*
 * Nobody has an empty CID, nickname or token, so an identifier that is absent
 * and one that is present but empty are equally useless for telling one peer or
 * one connection request from another.
 */
static bool identifies(const char* str) {
	return str && *str;
}

QuickDC::Clearance::Clearance(enum ClearanceType type_, enum ClearanceProtocol protocol_, QuickDC::HubSession* hubsession_, const char* userID_, const char* token_)
	: userID(userID_ ? userID_ : ""), token(token_ ? token_ : "")
{
	type = type_;
	protocol = protocol_;
	hubsession = hubsession_;

	QNET("Clearance: '%s' on hub '%p' (protocol=%s, authorization=%s, token=%s)",
		userID.c_str(),
		hubsession,
		(protocol == ADC) ?
			"ADC" :
			(protocol == NMDC) ?
				"NMDC" :
				"HTTP",
		(type == UnknownUser ? "everyone" : type == KnownUser ? "authenticated" : "exclusive"),
		token.c_str());
}

QuickDC::Clearance::~Clearance() {
}

bool QuickDC::Clearance::hasExpired() const {
	/* getInternalData() is the const way to read it; TimeStamp::elapsed() is not
	   const and this is asked of a clearance we are only looking at. */
	const time_t age = time(0) - timestamp.getInternalData();
	return age > QUICKDC_CLEARANCE_LIFETIME_SECONDS;
}

QuickDC::SecurityManager::SecurityManager() {

}

QuickDC::SecurityManager::~SecurityManager() {
	while (access_table.size()) {
		QuickDC::Clearance* authorization = access_table.back();
		delete authorization;
		access_table.pop_back();
	}
}


bool QuickDC::SecurityManager::isAuthorized(enum QuickDC::Clearance::ClearanceProtocol protocol, QuickDC::HubSession* hubsession, Samurai::IO::Net::Socket* socket, const char* cid, const char* token) {
	QDBG("Checking authorization for user '%s' (token=%s), hubsession=%p, protocol=%s",
		cid, token, hubsession, protocol == QuickDC::Clearance::ADC ? "ADC" : protocol == QuickDC::Clearance::NMDC ? "NMDC" : "HTTP");

#ifdef QUICKDC_ALLOW_LOOPBACK_AUTH
	/*
	 * Authorise anything arriving over the loopback, before any clearance, token
	 * or ID is looked at.
	 *
	 * This is a development convenience and nothing else. It means any process on
	 * this machine - any local account, and anything reached through an SSH or
	 * SOCKS forward to the peer port - can ask for and receive whatever this
	 * client would upload, without having been told about through a hub. It also
	 * makes the strength of a transfer token irrelevant locally, since the token
	 * is never reached.
	 *
	 * Compiled out unless QUICKDC_ALLOW_LOOPBACK_AUTH was asked for, which the
	 * build warns about. It used to be `#if 1`.
	 */
	if (socket) {
		/* getAddress() answers null for a socket that has no peer - one that
		   never connected, or was closed - and this used to dereference it. */
		const Samurai::IO::Net::InetAddress* addr = socket->getAddress();
		if (addr && addr->isLoopback()) {
			QERR("SECURITY: authorizing a loopback connection with no clearance, "
				"because this build has QUICKDC_ALLOW_LOOPBACK_AUTH");
			return true;
		}
	}
#else
	(void) socket;
#endif

	if (!access_table.size()) return false;

	std::vector<Clearance*>::iterator it = access_table.begin();
	for (; it != access_table.end(); it++) {
		QuickDC::Clearance* authorization = (*it);

		/*
		 * A clearance is granted for a connection we have just asked for, so one
		 * this old is not the connection it was granted for - and honouring it
		 * would leave the token that satisfies it worth guessing for as long as
		 * the process runs. Swept in grant() rather than removed here, which
		 * would invalidate the iterator this walk is holding.
		 */
		if (authorization->hasExpired()) continue;

		if (authorization->protocol == protocol && (authorization->hubsession == hubsession || !hubsession)) {
			if (authorization->type == QuickDC::Clearance::UnknownUser) {
				return true;
			}

			if (protocol == QuickDC::Clearance::ADC) {
				/* Both sides of each comparison, since a clearance granted
				   without one of them names no single connection request. */
				if (!identifies(token) || authorization->token.empty()) continue;
				if (!identifies(cid) || authorization->userID.empty()) continue;

				/*
				 * The token is compared without leaking where it first differs;
				 * the CID is not, because it is not a secret - every client on the
				 * hub is sent it in an INF. Timing the one that is public tells an
				 * attacker what they were already told.
				 */
				if (authorization->userID == cid
					&& Samurai::Util::constantTimeEquals(
						authorization->token.c_str(), token))
					return true;
			}

			/*
			 * NMDC names a peer by nickname and carries no token, so the nick is
			 * the whole of what can be matched. That is weak - the protocol
			 * neither encrypts nor authenticates it, so anyone who can reach the
			 * port and knows a nick we are waiting for satisfies this. It is
			 * still the difference between a connection we asked for and one
			 * that merely arrived, which is what decides whether a peer gets to
			 * ask for a file.
			 */
			if (protocol == QuickDC::Clearance::NMDC) {
				if (!identifies(cid) || authorization->userID.empty()) continue;
				if (authorization->userID == cid)
					return true;
			}
		}
	}
	return false;
}

size_t QuickDC::SecurityManager::expire() {
	size_t dropped = 0;

	std::vector<Clearance*>::iterator it = access_table.begin();
	while (it != access_table.end()) {
		QuickDC::Clearance* authorization = (*it);

		if (authorization->hasExpired()) {
			QDBG("Expiring the clearance for '%s'", authorization->userID.c_str());
			it = access_table.erase(it);
			delete authorization;
			dropped++;
		}
		else {
			it++;
		}
	}

	return dropped;
}

void QuickDC::SecurityManager::grant(Clearance* authorization) {
	if (!authorization) return;

	/* Swept here because this is the only thing that makes the table grow. */
	expire();

	/*
	 * A hub decides how many connections it asks us to make, so the table's size
	 * is not ours alone to control. The oldest goes rather than the newest: the
	 * newest is what the user just did, and a request old enough to be at the
	 * front of a full table is one whose peer has had every chance to arrive.
	 */
	while (access_table.size() >= QUICKDC_CLEARANCE_MAX) {
		QuickDC::Clearance* oldest = access_table.front();
		QERR("More than %d peer connections are pending clearance; dropping the "
			"oldest, for '%s'", QUICKDC_CLEARANCE_MAX, oldest->userID.c_str());
		access_table.erase(access_table.begin());
		delete oldest;
	}

	access_table.push_back(authorization);
}

void QuickDC::SecurityManager::revoke(Clearance* authorization) {
	if (!access_table.size()) return;

	std::vector<Clearance*>::iterator it = access_table.begin();
	for (; it != access_table.end(); it++) {
		if (*it == authorization) {
			access_table.erase(it);
			return;
		}
	}
}

bool QuickDC::SecurityManager::revoke(enum QuickDC::Clearance::ClearanceProtocol protocol,
	QuickDC::HubSession* hubsession, const char* cid, const char* token) {

	if (!identifies(cid)) return false;

	std::vector<Clearance*>::iterator it = access_table.begin();
	for (; it != access_table.end(); it++) {
		QuickDC::Clearance* authorization = (*it);

		if (authorization->protocol != protocol) continue;

		/* A null session matches any, as in isAuthorized(). */
		if (hubsession && authorization->hubsession != hubsession) continue;

		/* A clearance naming no user is tied to no single connection and must
		   survive; nothing grants one today, and one that appeared would be a
		   blanket permission rather than something to consume here. */
		if (authorization->userID.empty()) continue;
		if (authorization->userID != cid) continue;

		/*
		 * The token has to agree, and its absence has to agree too. NMDC carries
		 * no token, so a clearance granted for it has none and is matched on the
		 * nickname alone - but a caller naming no token must not then consume an
		 * ADC clearance that has one, or a nickname would revoke a transfer
		 * authorised by a secret.
		 */
		if (identifies(token) != !authorization->token.empty()) continue;
		if (identifies(token) && authorization->token != token) continue;

		QDBG("Revoking clearance for '%s' (token=%s)", cid, token ? token : "");
		access_table.erase(it);
		delete authorization;
		return true;
	}

	return false;
}

void QuickDC::SecurityManager::revoke(QuickDC::HubSession* hubsession) {
	/*
	 * erase() invalidates the iterator, so it cannot simply be incremented
	 * afterwards - which is what this used to do, walking off the end of the
	 * vector as soon as it matched anything. erase() hands back the next valid
	 * position instead, and only the non-matching case advances.
	 */
	std::vector<Clearance*>::iterator it = access_table.begin();
	while (it != access_table.end()) {
		QuickDC::Clearance* authorization = (*it);

		if (authorization->hubsession == hubsession) {
			it = access_table.erase(it);
			delete authorization;
		}
		else {
			it++;
		}
	}
}



