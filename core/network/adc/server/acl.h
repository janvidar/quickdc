/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCHUB_ACL_H
#define HAVE_QUICKDC_ADCHUB_ACL_H

#include "quickdc.h"
#include <samurai/timestamp.h>
#include "xml/reader.h"

#include <vector>


namespace ADC {
namespace ACL {

class Authorization;
class Table;

/**
 * A class representing a identification used in the authorization process.
 * A user can be identified by these criterias;
 * - IPv4 address
 * - IPv6 address
 * - Client ID
 * - Name
 */
class Identity {
	public:
		enum IdentityType { Invalid, IdIPv4, IdIPv6, IdCID, IdName };
		
		static enum IdentityType convertIdentityType(const char*);
		static const char* convertIdentityType(enum IdentityType );
		
	public:
		Identity(enum IdentityType type, const char* data);
		~Identity();
		
		enum IdentityType getType() const { return type; }
		
		bool matchCID(const char* cid);
		bool matchName(const char* name);
		
		// FIXME: Make sure we have a class for encapsulating IP addresses!
		bool matchIPv4();
		bool matchIPv6();
		
	
	protected:
		enum IdentityType type;
		char* data;

	friend class Authorization;
	friend class Table;
};

/**
 * This class represents a verification part of the authorization process.
 * Basically by verifying an Identity. An identity can be verified in these
 * ways;
 * - shared secret (password)
 * - cryptographic keys (certificates)
 * - none (only used for banned users).
 */
class Verification {
	public:
		enum VerificationType { Invalid, VerifyNone, VerifyPassword, VerifyCertificate };
		
		static enum VerificationType convertVerificationType(const char*);
		static const char* convertVerificationType(enum VerificationType);

	public:
		Verification(enum VerificationType type, const char* data);
		~Verification();
		
	protected:
		enum VerificationType type;
		char* data;
	
	friend class Authorization;
	friend class Table;
};

/**
 * An authorization consists of two parts, an identification and a verification.
 * Different types of authorizations exists:
 * - A banned user (registered user that is not allowed to log in)
 * - A guest (no identification or verification)
 * - A user (registered user, with no special rights)
 * - A "link" (kept for the future, for linking hubs together)
 * - A operator (user with the right to kick/ban/etc)
 * - An administrator (user with rights to everything. Including assigning credentials)
 */
class Authorization {
	public:
		enum Credential { Invalid, Banned, Guest, User, Link, Operator, Admin };
	
		static const char* convertCredential(enum Credential cred);
		static enum Credential convertCredential(const char*);
	
	public:
		Authorization(enum Credential cred, Samurai::TimeStamp expire, const char* name, const char* description);
		~Authorization();
		
		enum Credential getCredential() const { return credential; }
		const char* getName() const { return name; }
		const char* getDescription() const { return description; }
		bool isExpired();
		Samurai::TimeStamp getExpireTime() const { return expire; }
		
		void addIdentity(Identity* id);
		void addVerification(Verification* verification);
	
	protected:
		char* name;
		char* description;
		Samurai::TimeStamp expire;
		enum Credential credential;
		std::vector<Identity*> identities;
		std::vector<Verification*> verifications;

	friend class Table;
};


class Table {
	public:
		Table();
		virtual ~Table();
		
		void add(Authorization*);
		
		bool load();
		bool save();
		
	protected:
		std::vector<Authorization*> acl;
};

class XMLReader : public XML::SAX::ContentHandler {
	public:
		XMLReader(Table* acl, Samurai::IO::Buffer* buffer);
		virtual ~XMLReader();
		
		bool parse();
	
	public:
		void startDocument();
		void endDocument();
		void startElement(const char*, const char* localName, const char*, AttributeList list);
		void endElement(const char*, const char* localName, const char*);
		void characters(const char* ch, size_t, size_t);
		void ignorableWhitespace(const char*, size_t, size_t);
		void warning(const char* str);
		void error(const char* str);
		void fatalError(const char*);
		
	protected:
		void onProblem();
	
		XML::SAX::XMLReader* reader;
		bool problem;
		
	private:
		Samurai::IO::Buffer* buffer;
		Authorization* current;
		Table* acl;
		int level;
};

} // namespace ACL
} // namespace ADC


#endif // HAVE_QUICKDC_ADCHUB_ACL_H

