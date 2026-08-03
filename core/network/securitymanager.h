/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SECURITY_MANAGER_H
#define HAVE_QUICKDC_SECURITY_MANAGER_H

#include "quickdc.h"
#include <string>
#include <vector>

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
}

namespace QuickDC {

class HubSession;
class SecurityManager;

class Clearance {
	public:
		enum ClearanceType     { UnknownUser, KnownUser, AuthorizedUser };
		enum ClearanceProtocol { ADC, NMDC, HTTP };
		enum ClearanceAction   { Upload, Download };

		Clearance(enum ClearanceType, enum ClearanceProtocol, QuickDC::HubSession*, const char* userID, const char* token);
		virtual ~Clearance();

		Clearance(const Clearance&) = delete;
		Clearance& operator=(const Clearance&) = delete;
		
	protected:

		enum ClearanceType type;
		enum ClearanceProtocol protocol;
		enum ClearanceAction action;
		
		QuickDC::HubSession* hubsession;

		/*
		 * Empty means the clearance was granted without one. Neither identifies
		 * anything, so neither can be matched: see isAuthorized().
		 */
		std::string userID;
		std::string token;
		Samurai::TimeStamp timestamp;

	friend class SecurityManager;
};

class SecurityManager
{
	public:
		SecurityManager();
		~SecurityManager();

		SecurityManager(const SecurityManager&) = delete;
		SecurityManager& operator=(const SecurityManager&) = delete;

		void grant(Clearance* authorization);
		void revoke(Clearance* authorization);
		void revoke(QuickDC::HubSession*);

		/**
		 * Revoke the clearance that was granted for a single connection
		 * request, identified the same way isAuthorized() matches it.
		 *
		 * A clearance authorises one peer connection. Once that connection has
		 * arrived and been checked it has served its purpose, and leaving it in
		 * the table only makes the table grow for the lifetime of the process.
		 *
		 * @param session the hub session it was granted on, or 0 to match any.
		 * @return true if a matching clearance was found and removed.
		 */
		bool revoke(enum QuickDC::Clearance::ClearanceProtocol protocol,
			QuickDC::HubSession* session, const char* cid, const char* token);
		
		bool isAuthorized(enum QuickDC::Clearance::ClearanceProtocol protocol, QuickDC::HubSession* session, Samurai::IO::Net::Socket* socket, const char* cid, const char* token);

	protected:
		std::vector<Clearance*> access_table;

	
};

}

#endif // HAVE_QUICKDC_SECURITY_MANAGER_H
