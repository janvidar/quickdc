/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/securitymanager.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/net/inetaddress.h>
#include <stdlib.h>

QuickDC::Clearance::Clearance(enum ClearanceType type_, enum ClearanceProtocol protocol_, QuickDC::HubSession* hubsession_, const char* userID_, const char* token_) {
	type = type_;
	protocol = protocol_;
	hubsession = hubsession_;
	
	userID = 0;
	if (userID_)
		userID = strdup(userID_);

	token = 0;
	if (token_)
		token = strdup(token_);


	QNET("Clearance: '%s' on hub '%p' (protocol=%s, authorization=%s, token=%s)",
		userID,
		hubsession,
		(protocol == ADC) ?
			"ADC" :
			(protocol == NMDC) ?
				"NMDC" :
				"HTTP",
		(type == UnknownUser ? "everyone" : type == KnownUser ? "authenticated" : "exclusive"),
		token)
	
}

QuickDC::Clearance::~Clearance() {
	free(token);
	free(userID);
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
				if (!token || !authorization->token) continue;
				if (strcmp(authorization->token, token) == 0 &&
					strcmp(authorization->userID, cid) == 0)
					return true;
			}
#if 0
			if (protocol == QuickDC::Clearance::NMDC) {
				if (!token) continue;
			}

			if (authorization->type == QuickDC::Clearance::KnownUsers) {
				return true;
			}
#endif // 0
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

void QuickDC::SecurityManager::revoke(QuickDC::HubSession* hubsession) {
	if (!access_table.size()) return;

	std::vector<Clearance*>::iterator it = access_table.begin();
	for (; it != access_table.end(); it++) {
		QuickDC::Clearance* authorization = (*it);
		if (authorization->hubsession == hubsession) {
			access_table.erase(it);
			delete authorization;
		}
	}
}



