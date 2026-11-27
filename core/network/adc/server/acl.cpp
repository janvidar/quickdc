/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/adc/server/acl.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>

#include <algorithm>
#include <string_view>
#include <vector>
#include <time.h>

namespace XML
{
	extern void escape(Samurai::IO::Buffer* buffer, const std::string& str);
}


/* The name is whatever the caller was able to read from the file, and nothing at
   all is not a credential - no more than any other unrecognised name is. */
enum ADC::LocalHub::ACL::Authorization::Credential ADC::LocalHub::ACL::Authorization::convertCredential(const char* str)
{
	const std::string_view name(str ? str : "");

	if ( Samurai::Util::iequals(name, "banned") )
		return ADC::LocalHub::ACL::Authorization::Banned;

	if ( Samurai::Util::iequals(name, "guest") )
		return ADC::LocalHub::ACL::Authorization::Guest;

	if ( Samurai::Util::iequals(name, "user") )
		return ADC::LocalHub::ACL::Authorization::User;

	if ( Samurai::Util::iequals(name, "link") )
		return ADC::LocalHub::ACL::Authorization::Link;

	if ( Samurai::Util::iequals(name, "operator") )
		return ADC::LocalHub::ACL::Authorization::Operator;

	if ( Samurai::Util::iequals(name, "admin") )
		return ADC::LocalHub::ACL::Authorization::Admin;

	return ADC::LocalHub::ACL::Authorization::Invalid;
}

const char* ADC::LocalHub::ACL::Authorization::convertCredential(enum ADC::LocalHub::ACL::Authorization::Credential type)
{
	switch (type)
	{
		case ADC::LocalHub::ACL::Authorization::Banned:   return "banned";
		case ADC::LocalHub::ACL::Authorization::Guest:    return "guest";
		case ADC::LocalHub::ACL::Authorization::User:     return "user";
		case ADC::LocalHub::ACL::Authorization::Link:     return "link";
		case ADC::LocalHub::ACL::Authorization::Operator: return "operator";
		case ADC::LocalHub::ACL::Authorization::Admin:    return "admin";
		default:
			return "invalid";
	}
}

ADC::LocalHub::ACL::Authorization::Authorization(enum Credential cred_, Samurai::TimeStamp expire_, const char* name_, const char* description_)
	: name(name_ ? name_ : ""), description(description_ ? description_ : "")
{
	credential = cred_;
	expire = expire_;

	QDBG("* Creating authorization for user %s (%s). access=%s, expires=%s", name.c_str(), description.c_str(), convertCredential(credential), expire.getTime().c_str());
}

ADC::LocalHub::ACL::Authorization::~Authorization()
{
	while (identities.size())
	{
		ADC::LocalHub::ACL::Identity* id = identities.back();
		identities.pop_back();
		delete id;
	}

	while (verifications.size())
	{
		ADC::LocalHub::ACL::Verification* vrfy = verifications.back();
		verifications.pop_back();
		delete vrfy;
	}
}

bool ADC::LocalHub::ACL::Authorization::isExpired() const
{
	const time_t at = expire.getInternalData();
	return at != 0 && at <= time(0);
}

bool ADC::LocalHub::ACL::Authorization::identifies(const char* cid, const char* nick, const std::string& address) const
{
	for (const Identity* id : identities)
		if (id->matchCID(cid) || id->matchName(nick) || id->matchAddress(address)) return true;
	return false;
}

const ADC::LocalHub::ACL::Verification* ADC::LocalHub::ACL::Authorization::password() const
{
	for (const Verification* verification : verifications)
		if (verification->getType() == Verification::VerifyPassword && !verification->getData().empty())
			return verification;
	return 0;
}


void ADC::LocalHub::ACL::Authorization::addIdentity(ADC::LocalHub::ACL::Identity* id)
{
	QDBG("> Identity for user %s: %s (%s)", name.c_str(), id->data.c_str(), Identity::convertIdentityType(id->type));
	identities.push_back(id);
}

void ADC::LocalHub::ACL::Authorization::addVerification(ADC::LocalHub::ACL::Verification* verification)
{
	QDBG("> Verification for user %s: %s (%s)", name.c_str(), verification->data.c_str(), Verification::convertVerificationType(verification->type));
	verifications.push_back(verification);
}

/* ------------------------------------------------------------------------- */

enum ADC::LocalHub::ACL::Identity::IdentityType ADC::LocalHub::ACL::Identity::convertIdentityType(const char* str)
{
	const std::string_view name(str ? str : "");

	if ( Samurai::Util::iequals(name, "name") )
		return ADC::LocalHub::ACL::Identity::IdName;

	if ( Samurai::Util::iequals(name, "cid") )
		return ADC::LocalHub::ACL::Identity::IdCID;

	if ( Samurai::Util::iequals(name, "ipv4") )
		return ADC::LocalHub::ACL::Identity::IdIPv4;

	if ( Samurai::Util::iequals(name, "ipv6") )
		return ADC::LocalHub::ACL::Identity::IdIPv6;

	return ADC::LocalHub::ACL::Identity::Invalid;
}

const char* ADC::LocalHub::ACL::Identity::convertIdentityType(enum ADC::LocalHub::ACL::Identity::IdentityType type)
{
	switch (type)
	{
		case ADC::LocalHub::ACL::Identity::IdName: return "name";
		case ADC::LocalHub::ACL::Identity::IdCID:  return "cid";
		case ADC::LocalHub::ACL::Identity::IdIPv4: return "ipv4";
		case ADC::LocalHub::ACL::Identity::IdIPv6: return "ipv6";
		default:
			return "invalid";
	}
}

ADC::LocalHub::ACL::Identity::Identity(enum IdentityType type_, const char* data_)
	: data(data_ ? data_ : "")
{
	type = type_;
}

ADC::LocalHub::ACL::Identity::~Identity()
{
}

bool ADC::LocalHub::ACL::Identity::matchCID(const char* cid) const
{
	return type == IdCID && cid && *cid && data == cid;
}

bool ADC::LocalHub::ACL::Identity::matchName(const char* name) const
{
	return type == IdName && name && *name && Samurai::Util::iequals(data, name);
}

bool ADC::LocalHub::ACL::Identity::matchAddress(const std::string& address) const
{
	return (type == IdIPv4 || type == IdIPv6) && !address.empty() && data == address;
}


/* ------------------------------------------------------------------------- */

enum ADC::LocalHub::ACL::Verification::VerificationType ADC::LocalHub::ACL::Verification::convertVerificationType(const char* str)
{
	const std::string_view name(str ? str : "");

	if ( Samurai::Util::iequals(name, "none") )
		return ADC::LocalHub::ACL::Verification::VerifyNone;

	if ( Samurai::Util::iequals(name, "password") )
		return ADC::LocalHub::ACL::Verification::VerifyPassword;

	if ( Samurai::Util::iequals(name, "certificate") )
		return ADC::LocalHub::ACL::Verification::VerifyCertificate;

	return ADC::LocalHub::ACL::Verification::Invalid;
}

const char* ADC::LocalHub::ACL::Verification::convertVerificationType(enum ADC::LocalHub::ACL::Verification::VerificationType type)
{
	switch (type)
	{
		case ADC::LocalHub::ACL::Verification::VerifyNone: return "none";
		case ADC::LocalHub::ACL::Verification::VerifyPassword: return "password";
		case ADC::LocalHub::ACL::Verification::VerifyCertificate: return "certificate";
		default:
			return "invalid";
	}
}

ADC::LocalHub::ACL::Verification::Verification(enum ADC::LocalHub::ACL::Verification::VerificationType type_, const char* data_)
	: data(data_ ? data_ : "")
{
	type = type_;
}

ADC::LocalHub::ACL::Verification::~Verification()
{
}

/* ------------------------------------------------------------------------- */


ADC::LocalHub::ACL::Table::Table(const char* path_)
	: path(path_ ? path_ : "")
{
	load();
}

ADC::LocalHub::ACL::Table::~Table()
{
	if (changed) save();

	while (acl.size())
	{
		ADC::LocalHub::ACL::Authorization* auth = acl.back();
		acl.pop_back();
		delete auth;
	}
}

void ADC::LocalHub::ACL::Table::add(ADC::LocalHub::ACL::Authorization* auth)
{
	acl.push_back(auth);
	changed = true;
}

void ADC::LocalHub::ACL::Table::remove(const ADC::LocalHub::ACL::Authorization* auth)
{
	const auto found = std::find(acl.begin(), acl.end(), auth);
	if (found == acl.end()) return;

	delete *found;
	acl.erase(found);
	changed = true;
}

const ADC::LocalHub::ACL::Authorization* ADC::LocalHub::ACL::Table::banFor(const char* cid, const char* nick,
	const std::string& address) const
{
	for (const Authorization* auth : acl)
		if (auth->credential == Authorization::Banned && !auth->isExpired()
		    && auth->identifies(cid, nick, address))
			return auth;
	return 0;
}

const ADC::LocalHub::ACL::Authorization* ADC::LocalHub::ACL::Table::accountFor(const char* cid, const char* nick) const
{
	for (const Authorization* auth : acl)
	{
		if (auth->credential != Authorization::User && auth->credential != Authorization::Operator
		    && auth->credential != Authorization::Admin) continue;
		if (auth->isExpired() || !auth->password()) continue;
		if (auth->identifies(cid, nick, std::string())) return auth;
	}
	return 0;
}

/*
 * An access attributes is a hand-written file of a few entries per registered user,
 * so anything past this is a corrupt or hostile file rather than a large one,
 * and allocating from what it claims would be taking its word for it.
 */
#define MAX_ACL_SIZE (4 * 1024 * 1024)

bool ADC::LocalHub::ACL::Table::load()
{
	QDBG("Loading acl");
	Samurai::IO::File file(path);

	if (!file.exists())
	{
		QDBG("No access attributes on disk yet");
		return true;
	}

	/*
	 * size() before open() answers from a stat that may never have been taken,
	 * and an empty file then left new[] with nothing to hand Buffer::append(),
	 * which indexes its first element to find the destination.
	 */
	if (!file.open(Samurai::IO::File::Mode::Read))
	{
		QERR("Unable to open file");
		return false;
	}

	const off_t reported = file.size();
	if (reported <= 0)
	{
		QDBG("The access attributes on disk is empty");
		file.close();
		return true;
	}

	if (reported > MAX_ACL_SIZE)
	{
		QERR("The access attributes is %ld bytes, which is more than an access attributes can be",
			static_cast<long>(reported));
		file.close();
		return false;
	}

	// Read file into memory
	const size_t size = static_cast<size_t>(reported);
	uint8_t* temp_buffer = new uint8_t[size];
	const ssize_t got = file.read(reinterpret_cast<char*>(temp_buffer), size);
	file.close();

	/* What was read, not what was asked for: the tail of a short read is
	   uninitialised, and handing it to the parser reads whatever it holds. */
	if (got <= 0)
	{
		QERR("Unable to read the access attributes");
		delete[] temp_buffer;
		return false;
	}

	Samurai::IO::Buffer* buffer = new Samurai::IO::Buffer(static_cast<size_t>(got));
	buffer->append(reinterpret_cast<char*>(temp_buffer), static_cast<size_t>(got));
	delete[] temp_buffer;

	ADC::LocalHub::ACL::XMLReader* handler = new ADC::LocalHub::ACL::XMLReader(this, buffer);
	bool ret = handler->parse();

	delete handler;
	delete buffer;

	return ret;
}

#define NEWLINE "\r\n"
#define INDENT '\x9'
#define SEPARATOR '|'

#define XML_NEWLINE "\r\n"
#define XML_INDENT "\x9"

bool ADC::LocalHub::ACL::Table::save() {
	QDBG("Saving acl");
	
	Samurai::IO::Buffer* xmlout = new Samurai::IO::Buffer();
	xmlout->append("<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>" XML_NEWLINE);
	xmlout->append("<AccessList version=\"1\" generator=\"" PRODUCT "/" VERSION " (" SYSTEM "/" CPU ")\" updated=\"");
	xmlout->append(std::to_string(static_cast<int64_t>(Samurai::TimeStamp().getInternalData())));
	xmlout->append("\">" XML_NEWLINE);

	std::vector<ADC::LocalHub::ACL::Authorization*>::iterator it;
	for (it = acl.begin(); it != acl.end(); it++) {
		ADC::LocalHub::ACL::Authorization* auth = (*it);
		xmlout->append(XML_INDENT "<User credential=\"");
		xmlout->append(ADC::LocalHub::ACL::Authorization::convertCredential(auth->credential));
		xmlout->append("\" name=\"");
		XML::escape(xmlout, auth->name);
		xmlout->append("\" description=\"");
		XML::escape(xmlout, auth->description);
		xmlout->append("\" expire=\"");
		xmlout->append(std::to_string(static_cast<int64_t>(auth->expire.getInternalData())));
		xmlout->append("\">" XML_NEWLINE);

		
		for (std::vector<ADC::LocalHub::ACL::Identity*>::iterator it2 = auth->identities.begin(); it2 != auth->identities.end(); it2++) {
			ADC::LocalHub::ACL::Identity* id = (*it2);
			xmlout->append(XML_INDENT XML_INDENT "<id type=\"");
			xmlout->append(ADC::LocalHub::ACL::Identity::convertIdentityType(id->type));
			xmlout->append("\" data=\"");
			XML::escape(xmlout, id->data);
			xmlout->append("\" />" XML_NEWLINE);
		}

		for (std::vector<ADC::LocalHub::ACL::Verification*>::iterator it2 = auth->verifications.begin(); it2 != auth->verifications.end(); it2++) {
			ADC::LocalHub::ACL::Verification* vrfy = (*it2);
			xmlout->append(XML_INDENT XML_INDENT "<verify type=\"");
			xmlout->append(ADC::LocalHub::ACL::Verification::convertVerificationType(vrfy->type));
			xmlout->append("\" data=\"");
			XML::escape(xmlout, vrfy->data);
			xmlout->append("\" />" XML_NEWLINE);
		}
		xmlout->append(XML_INDENT "</User>" XML_NEWLINE);
	}
	
	xmlout->append("</AccessList>" XML_NEWLINE);
	
	/*
	 * Passwords are kept as given: ADC's GPA/PAS proves a password by the tiger hash
	 * of it and a salt the hub chooses each time, so the hub needs the password
	 * itself, and no stored digest could answer a fresh salt. So the file, and the
	 * directory it is in, are its owner's alone.
	 */
	Samurai::IO::File xmlacl(path);
	bool ret = xmlacl.open(Samurai::IO::File::Mode::Write | Samurai::IO::File::Mode::Truncate
	                       | Samurai::IO::File::Mode::Private);
	if (ret) {
		char* out = new char[xmlout->size()];
		xmlout->pop(out, xmlout->size());
		int written = xmlacl.write(out, xmlout->size());
		if (written == -1) QDBG("Error writing acl.xml");
		else changed = false;
		xmlacl.close();
		delete[] out;
	}

	delete xmlout;
	
	return ret;
}

/* ------------------------------------------------------------------------- */

ADC::LocalHub::ACL::XMLReader::XMLReader(ADC::LocalHub::ACL::Table* acl_, Samurai::IO::Buffer* buffer) {
	acl = acl_;
	source = buffer;
	current = 0;
	problem = false;
}

ADC::LocalHub::ACL::XMLReader::~XMLReader()
{
	/* A file that stops in the middle of a <User> leaves one built but never
	   handed to the table. */
	delete current;
}

bool ADC::LocalHub::ACL::XMLReader::parse()
{
	problem = false;
	if (!source) return false;

	/*
	 * The hub's own access attributes, which an administrator writes. Bounded anyway:
	 * this decides who gets in, so a file that is not what it should be is one to
	 * refuse rather than to read as far as it goes.
	 */
	Samurai::IO::XmlReader::Limits limits;
	limits.maxDocumentSize = QUICKDC_ACL_MAX_SIZE;
	limits.maxDepth        = 8;
	limits.maxElements     = QUICKDC_ACL_MAX_ELEMENTS;
	limits.maxAttributes   = 32;
	limits.maxTextLength   = 4096;

	const Samurai::IO::XmlError parsed = Samurai::IO::XmlReader::parse(
		std::string_view(source->ptr(), source->size()), *this, limits);

	if (parsed != Samurai::IO::XmlError::Ok)
		fatalError(Samurai::IO::toString(parsed));

	return !problem;
}

void ADC::LocalHub::ACL::XMLReader::startElement(std::string_view, std::string_view element,
	const Attributes& attributes)
{

	if (Samurai::Util::iequals(element, "AccessList"))
	{
		if (current)
		{
			onProblem();
			return;
		}
	}
	else if (Samurai::Util::iequals(element, "User"))
	{
		if (current)
		{
			onProblem();
			return;
		}
		
		const char* s_credential = 0;
		const char* s_name = 0;
		const char* s_description = 0;
		const char* s_expire = 0;
		
		for (size_t n = 0; n < attributes.size(); n++)
		{
			if (Samurai::Util::iequals(attributes[n].first, "name"))
				s_name = attributes[n].second.c_str();
			else if (Samurai::Util::iequals(attributes[n].first, "description"))
				s_description = attributes[n].second.c_str();
			else if (Samurai::Util::iequals(attributes[n].first, "credential"))
				s_credential = attributes[n].second.c_str();
			else if (Samurai::Util::iequals(attributes[n].first, "expire"))
				s_expire = attributes[n].second.c_str();
		}
		
		if (!(s_name && *s_name && s_description && *s_description
			&& s_credential && *s_credential && s_expire && *s_expire))
		{
			fatalError("Missing/invalid parameters in user");
			return;
		}
			
		enum ADC::LocalHub::ACL::Authorization::Credential credential = ADC::LocalHub::ACL::Authorization::convertCredential(s_credential);
		if (credential == ADC::LocalHub::ACL::Authorization::Invalid)
		{
			error("Invalid credentials for user");
		}
		
		Samurai::TimeStamp expire = Samurai::TimeStamp(Samurai::Util::Convert::to_int64(s_expire));
		if (expire.getInternalData() != 0 && expire < Samurai::TimeStamp())
		{
			warning("Authorization has expired");
		}
		
		current = new Authorization(credential, expire, s_name, s_description);
		
	}
	else if (Samurai::Util::iequals(element, "id") || Samurai::Util::iequals(element, "verify"))
	{
		if (!current)
		{
			onProblem();
			return;
		}

		bool is_id = Samurai::Util::iequals(element, "id");
	
		const char* s_type = 0;
		const char* s_data = 0;

		for (size_t n = 0; n < attributes.size(); n++)
		{
			if (Samurai::Util::iequals(attributes[n].first, "type"))
				s_type = attributes[n].second.c_str();
			else if (Samurai::Util::iequals(attributes[n].first, "data"))
				s_data = attributes[n].second.c_str();
		}
		
		if (!(s_type && *s_type && s_data && *s_data))
		{
			fatalError("Missing/invalid parameters in id/verify");
			return;
		}
		
		if (is_id)
		{
			enum ADC::LocalHub::ACL::Identity::IdentityType type = ADC::LocalHub::ACL::Identity::convertIdentityType(s_type);
			if (type == ADC::LocalHub::ACL::Identity::Invalid)
			{
				error("Invalid identity type");
				return;
			}
			
			ADC::LocalHub::ACL::Identity* id = new ADC::LocalHub::ACL::Identity(type, s_data);
			current->addIdentity(id);
		}
		else
		{
			enum ADC::LocalHub::ACL::Verification::VerificationType type = ADC::LocalHub::ACL::Verification::convertVerificationType(s_type);
			if (type == ADC::LocalHub::ACL::Verification::Invalid)
			{
				error("Invalid verification type");
				return;
			}
			ADC::LocalHub::ACL::Verification* vrfy = new ADC::LocalHub::ACL::Verification(type, s_data);
			current->addVerification(vrfy);
		}
	}
}

void ADC::LocalHub::ACL::XMLReader::endElement(std::string_view, std::string_view element)
{
	if (Samurai::Util::iequals(element, "User"))
	{
		/* A <User> the start tag refused leaves nothing to add, and the table
		   walks its entries without checking them. */
		if (current)
		{
			acl->add(current);
			current = 0;
		}
	}
}

void ADC::LocalHub::ACL::XMLReader::characters(std::string_view)
{
}

void ADC::LocalHub::ACL::XMLReader::onProblem()
{
	problem = true;
}

/* ------------------------------------------------------------------------- */



