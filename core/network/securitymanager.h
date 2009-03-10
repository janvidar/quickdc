/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SECURITY_MANAGER_H
#define HAVE_QUICKDC_SECURITY_MANAGER_H

#include "quickdc.h"
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
		
	protected:

		enum ClearanceType type;
		enum ClearanceProtocol protocol;
		enum ClearanceAction action;
		
		QuickDC::HubSession* hubsession;
		char* userID;
		char* token;
		Samurai::TimeStamp timestamp;

	friend class SecurityManager;
};

class SecurityManager
{
	public:
		SecurityManager();
		~SecurityManager();

		void grant(Clearance* authorization);
		void revoke(Clearance* authorization);
		void revoke(QuickDC::HubSession*);
		
		bool isAuthorized(enum QuickDC::Clearance::ClearanceProtocol protocol, QuickDC::HubSession* session, Samurai::IO::Net::Socket* socket, const char* cid, const char* token);

	protected:
		std::vector<Clearance*> access_table;

	
};

}

#endif // HAVE_QUICKDC_SECURITY_MANAGER_H
