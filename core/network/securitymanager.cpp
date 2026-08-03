/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/securitymanager.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <stdlib.h>

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

#if 1
	/*
		Always allow things to happen locally -- useful for testing
		FIXME: Should this only be enabled in debug builds?
	*/
	if (socket) {
		const Samurai::IO::Net::InetAddress* addr = socket->getAddress();
		if (addr->isLoopback()) {
			QDBG("SECURITY NOTE: Authorizing link local connection");
			return true;
		}
	}
#endif

	if (!access_table.size()) return false;

	std::vector<Clearance*>::iterator it = access_table.begin();
	for (; it != access_table.end(); it++) {
		QuickDC::Clearance* authorization = (*it);
		if (authorization->protocol == protocol && (authorization->hubsession == hubsession || !hubsession)) {
			if (authorization->type == QuickDC::Clearance::UnknownUser) {
				return true;
			}

			if (protocol == QuickDC::Clearance::ADC) {
				/* Both sides of each comparison, since a clearance granted
				   without one of them names no single connection request. */
				if (!identifies(token) || authorization->token.empty()) continue;
				if (!identifies(cid) || authorization->userID.empty()) continue;
				if (authorization->token == token && authorization->userID == cid)
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

void QuickDC::SecurityManager::grant(Clearance* authorization) {
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

	if (!identifies(cid) || !identifies(token)) return false;

	std::vector<Clearance*>::iterator it = access_table.begin();
	for (; it != access_table.end(); it++) {
		QuickDC::Clearance* authorization = (*it);

		if (authorization->protocol != protocol) continue;

		/* A null session matches any, as in isAuthorized(). */
		if (hubsession && authorization->hubsession != hubsession) continue;

		/* Only a clearance naming this user and token; a blanket one is not
		   tied to a single connection and must survive. */
		if (authorization->token.empty() || authorization->userID.empty()) continue;

		if (authorization->token == token && authorization->userID == cid) {
			QDBG("Revoking clearance for '%s' (token=%s)", cid, token);
			access_table.erase(it);
			delete authorization;
			return true;
		}
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



