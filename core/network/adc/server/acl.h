/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADC_LOCALHUB_ACL_H
#define HAVE_QUICKDC_ADC_LOCALHUB_ACL_H

#include "quickdc.h"
#include <samurai/timestamp.h>
#include <samurai/io/xml.h>

#include <string>
#include <string_view>
#include <vector>

namespace Samurai {
	namespace IO {
		class Buffer;
	}
}

/**
 * Bounds on the hub's access list.
 *
 * Written by whoever administers the hub, and read to decide who gets in - so a
 * file that is not what it should be is refused rather than read as far as it
 * goes. Generous against any real list.
 */
#define QUICKDC_ACL_MAX_SIZE     (8*1024*1024)
#define QUICKDC_ACL_MAX_ELEMENTS (100*1000)


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
		
	
	protected:
		enum IdentityType type;
		std::string data;

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
		std::string data;

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

		Authorization(const Authorization&) = delete;
		Authorization& operator=(const Authorization&) = delete;
		
		enum Credential getCredential() const { return credential; }

		/**
		 * @return the name, or null when the authorization carries none. An
		 *         entry without one is refused when the table is read, so an
		 *         empty name is an absent one.
		 */
		const char* getName() const { return name.empty() ? 0 : name.c_str(); }
		const char* getDescription() const { return description.empty() ? 0 : description.c_str(); }
		bool isExpired();
		Samurai::TimeStamp getExpireTime() const { return expire; }
		
		void addIdentity(Identity* id);
		void addVerification(Verification* verification);
	
	protected:
		std::string name;
		std::string description;
		Samurai::TimeStamp expire;
		enum Credential credential;
		std::vector<Identity*> identities;
		std::vector<Verification*> verifications;

	friend class Table;
};


class Table {
	public:
		/**
		 * @param path_ where the table is read from and written back to, so it
		 *        can be pointed somewhere other than the real one.
		 */
		Table(const char* path_ = "~/.quickdc/hub/acl.xml");
		virtual ~Table();

		Table(const Table&) = delete;
		Table& operator=(const Table&) = delete;

		void add(Authorization*);

		bool load();
		bool save();

		size_t countUsers() const { return acl.size(); }
		const Authorization* getUser(size_t n) const { return (n < acl.size()) ? acl[n] : 0; }

	protected:
		std::vector<Authorization*> acl;
		std::string path;
};

class XMLReader : public Samurai::IO::XmlContentHandler {
	public:
		XMLReader(Table* acl, Samurai::IO::Buffer* buffer);
		~XMLReader() override;

		XMLReader(const XMLReader&) = delete;
		XMLReader& operator=(const XMLReader&) = delete;
		
		bool parse();
	
	public:
		void startElement(std::string_view, std::string_view element,
			const Attributes& attributes) override;
		void endElement(std::string_view, std::string_view element) override;
		void characters(std::string_view) override;
		
	protected:
		/*
		 * A complaint about what an entry says rather than about the markup. The
		 * entry is dropped and the file is still read: one unusable line in an
		 * access list is not a reason to admit nobody.
		 */
		void warning(const char* str) { QERR("ACL WARNING: %s", str); }
		void error(const char* str)   { QERR("ACL ERROR:   %s", str); }
		void fatalError(const char* str) { QERR("ACL ERROR:   %s", str); onProblem(); }

		void onProblem();
	
		/* Borrowed, and read when parse() is called. */
		Samurai::IO::Buffer* source;
		bool problem;
		
	private:
		Authorization* current;
		Table* acl;
};

} // namespace ACL
} // namespace ADC


#endif // HAVE_QUICKDC_ADC_LOCALHUB_ACL_H

