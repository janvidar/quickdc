/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/adc/server/acl.h"
#include "xml/reader.h"

#include <samurai/io/file.h>
#include <samurai/io/buffer.h>

#include <vector>

namespace XML
{
	extern void escape(Samurai::IO::Buffer* buffer, const char* str);
}


enum ADC::ACL::Authorization::Credential ADC::ACL::Authorization::convertCredential(const char* str)
{
	if ( !strcasecmp(str, "banned") )
		return ADC::ACL::Authorization::Banned;

	if ( !strcasecmp(str, "guest") )
		return ADC::ACL::Authorization::Guest;

	if ( !strcasecmp(str, "user") )
		return ADC::ACL::Authorization::User;

	if ( !strcasecmp(str, "link") )
		return ADC::ACL::Authorization::Link;

	if ( !strcasecmp(str, "operator") )
		return ADC::ACL::Authorization::Operator;

	if ( !strcasecmp(str, "admin") )
		return ADC::ACL::Authorization::Admin;

	return ADC::ACL::Authorization::Invalid;
}

const char* ADC::ACL::Authorization::convertCredential(enum ADC::ACL::Authorization::Credential type)
{
	switch (type)
	{
		case ADC::ACL::Authorization::Banned:   return "banned";
		case ADC::ACL::Authorization::Guest:    return "guest";
		case ADC::ACL::Authorization::User:     return "user";
		case ADC::ACL::Authorization::Link:     return "link";
		case ADC::ACL::Authorization::Operator: return "operator";
		case ADC::ACL::Authorization::Admin:    return "admin";
		default:
			return "invalid";
	}
}

ADC::ACL::Authorization::Authorization(enum Credential cred_, Samurai::TimeStamp expire_, const char* name_, const char* description_)
{
	credential = cred_;
	expire = expire_;
	name = 0;
	if (name_) name = strdup(name_);
	description = 0;
	if (description_) description = strdup(description_);

	QDBG("* Creating authorization for user %s (%s). access=%s, expires=%s", name, description, convertCredential(credential), expire.getTime());
}

ADC::ACL::Authorization::~Authorization()
{
	free(name);
	free(description);

	while (identities.size())
	{
		ADC::ACL::Identity* id = identities.back();
		identities.pop_back();
		delete id;
	}

	while (verifications.size())
	{
		ADC::ACL::Verification* vrfy = verifications.back();
		verifications.pop_back();
		delete vrfy;
	}
}

bool ADC::ACL::Authorization::isExpired()
{
	return expire > Samurai::TimeStamp();
}


void ADC::ACL::Authorization::addIdentity(ADC::ACL::Identity* id)
{
	QDBG("> Identity for user %s: %s (%s)", name, id->data, Identity::convertIdentityType(id->type));
	identities.push_back(id);
}

void ADC::ACL::Authorization::addVerification(ADC::ACL::Verification* verification)
{
	QDBG("> Verification for user %s: %s (%s)", name, verification->data, Verification::convertVerificationType(verification->type));
	verifications.push_back(verification);
}

/* ------------------------------------------------------------------------- */

enum ADC::ACL::Identity::IdentityType ADC::ACL::Identity::convertIdentityType(const char* str)
{
	if ( !strcasecmp(str, "name") )
		return ADC::ACL::Identity::IdName;

	if ( !strcasecmp(str, "cid") )
		return ADC::ACL::Identity::IdCID;

	if ( !strcasecmp(str, "ipv4") )
		return ADC::ACL::Identity::IdIPv4;

	if ( !strcasecmp(str, "ipv6") )
		return ADC::ACL::Identity::IdIPv6;

	return ADC::ACL::Identity::Invalid;
}

const char* ADC::ACL::Identity::convertIdentityType(enum ADC::ACL::Identity::IdentityType type)
{
	switch (type)
	{
		case ADC::ACL::Identity::IdName: return "name";
		case ADC::ACL::Identity::IdCID:  return "cid";
		case ADC::ACL::Identity::IdIPv4: return "ipv4";
		case ADC::ACL::Identity::IdIPv6: return "ipv6";
		default:
			return "invalid";
	}
}

ADC::ACL::Identity::Identity(enum IdentityType type_, const char* data_)
{
	type = type_;
	data = 0;
	if (data_) data = strdup(data_);
}

ADC::ACL::Identity::~Identity()
{
	free(data);
}

bool ADC::ACL::Identity::matchCID(const char*)
{
	QDBG("Implement me!");
	return false;
}

bool ADC::ACL::Identity::matchName(const char*)
{
	QDBG("Implement me!");
	return false;
}


/* ------------------------------------------------------------------------- */

enum ADC::ACL::Verification::VerificationType ADC::ACL::Verification::convertVerificationType(const char* str)
{
	if ( !strcasecmp(str, "none") )
		return ADC::ACL::Verification::VerifyNone;

	if ( !strcasecmp(str, "password") )
		return ADC::ACL::Verification::VerifyPassword;

	if ( !strcasecmp(str, "certificate") )
		return ADC::ACL::Verification::VerifyCertificate;

	return ADC::ACL::Verification::Invalid;
}

const char* ADC::ACL::Verification::convertVerificationType(enum ADC::ACL::Verification::VerificationType type)
{
	switch (type)
	{
		case ADC::ACL::Verification::VerifyNone: return "none";
		case ADC::ACL::Verification::VerifyPassword: return "password";
		case ADC::ACL::Verification::VerifyCertificate: return "certificate";
		default:
			return "invalid";
	}
}

ADC::ACL::Verification::Verification(enum ADC::ACL::Verification::VerificationType type_, const char* data_)
{
	type = type_;
	data = 0;
	if (data_) data = strdup(data_);
}

ADC::ACL::Verification::~Verification()
{
	free(data);
}

/* ------------------------------------------------------------------------- */


ADC::ACL::Table::Table()
{
	load();
}

ADC::ACL::Table::~Table()
{
	save();

	while (acl.size())
	{
		ADC::ACL::Authorization* auth = acl.back();
		acl.pop_back();
		delete auth;
	}
}

void ADC::ACL::Table::add(ADC::ACL::Authorization* auth)
{
	acl.push_back(auth);
}

bool ADC::ACL::Table::load()
{
	QDBG("Loading acl");
	Samurai::IO::File file("~/.quickdc/hub/acl.xml");
	
	size_t size = (size_t) file.size();
	if (!file.open(Samurai::IO::File::Read))
	{
		QERR("Unable to open file");
		return false;
	}

	// Read file into memory
	uint8_t* temp_buffer = new uint8_t[size];
	file.read((char*) temp_buffer, size);
	Samurai::IO::Buffer* buffer = new Samurai::IO::Buffer(size);
	buffer->append((char*) temp_buffer, size);
	delete[] temp_buffer;
	file.close();
	
	ADC::ACL::XMLReader* handler = new ADC::ACL::XMLReader(this, buffer);
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

bool ADC::ACL::Table::save() {
	QDBG("Saving acl");
	
	Samurai::IO::Buffer* xmlout = new Samurai::IO::Buffer();
	xmlout->append("<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>" XML_NEWLINE);
	xmlout->append("<AccessList version=\"1\" generator=\"" PRODUCT "/" VERSION " (" SYSTEM "/" CPU ")\" updated=\"");
	xmlout->append(Samurai::TimeStamp().getTime("%s"));
	xmlout->append("\">" XML_NEWLINE);

	std::vector<ADC::ACL::Authorization*>::iterator it;
	for (it = acl.begin(); it != acl.end(); it++) {
		ADC::ACL::Authorization* auth = (*it);
		xmlout->append(XML_INDENT "<User credential=\"");
		xmlout->append(ADC::ACL::Authorization::convertCredential(auth->credential));
		xmlout->append("\" name=\"");
		XML::escape(xmlout, auth->name);
		xmlout->append("\" description=\"");
		XML::escape(xmlout, auth->description);
		xmlout->append("\" expire=\"");
		xmlout->append(auth->expire.getTime("%s"));
		xmlout->append("\">" XML_NEWLINE);

		
		for (std::vector<ADC::ACL::Identity*>::iterator it2 = auth->identities.begin(); it2 != auth->identities.end(); it2++) {
			ADC::ACL::Identity* id = (*it2);
			xmlout->append(XML_INDENT XML_INDENT "<id type=\"");
			xmlout->append(ADC::ACL::Identity::convertIdentityType(id->type));
			xmlout->append("\" data=\"");
			XML::escape(xmlout, id->data);
			xmlout->append("\" />" XML_NEWLINE);
		}

		for (std::vector<ADC::ACL::Verification*>::iterator it2 = auth->verifications.begin(); it2 != auth->verifications.end(); it2++) {
			ADC::ACL::Verification* vrfy = (*it2);
			xmlout->append(XML_INDENT XML_INDENT "<verify type=\"");
			xmlout->append(ADC::ACL::Verification::convertVerificationType(vrfy->type));
			xmlout->append("\" data=\"");
			XML::escape(xmlout, vrfy->data);
			xmlout->append("\" />" XML_NEWLINE);
		}
		xmlout->append(XML_INDENT "</User>" XML_NEWLINE);
	}
	
	xmlout->append("</AccessList>" XML_NEWLINE);
	
	Samurai::IO::File xmlacl("~/.quickdc/hub/acl.xml");
	bool ret = xmlacl.open(Samurai::IO::File::Write | Samurai::IO::File::Truncate);
	if (ret) {
		char* out = new char[xmlout->size()];
		xmlout->pop(out, xmlout->size());
		int written = xmlacl.write(out, xmlout->size());
		if (written == -1) QDBG("Error writing acl.xml");
		xmlacl.close();
		delete[] out;
	}

	delete xmlout;
	
	return ret;
}

/* ------------------------------------------------------------------------- */

ADC::ACL::XMLReader::XMLReader(ADC::ACL::Table* acl_, Samurai::IO::Buffer* buffer) {
	acl = acl_;
	reader = new XML::SAX::XMLReader(buffer, this);
	current = 0;
}

ADC::ACL::XMLReader::~XMLReader()
{
	delete reader;
}

bool ADC::ACL::XMLReader::parse()
{
	problem = false;
	reader->parse();
	return !problem;
}

void ADC::ACL::XMLReader::startDocument()
{

}

void ADC::ACL::XMLReader::endDocument()
{

}

void ADC::ACL::XMLReader::startElement(const char*, const char* localName, const char*, AttributeList list)
{
	if (strcasecmp(localName, "AccessList") == 0)
	{
		if (current)
		{
			onProblem();
			return;
		}
	}
	else if (strcasecmp(localName, "User") == 0)
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
		
		for (size_t n = 0; n < list.size(); n++)
		{
			if (!strcasecmp(list[n].first, "name"))
				s_name = list[n].second;
			else if (!strcasecmp(list[n].first, "description"))
				s_description = list[n].second;
			else if (!strcasecmp(list[n].first, "credential"))
				s_credential = list[n].second;
			else if (!strcasecmp(list[n].first, "expire"))
				s_expire = list[n].second;
		}
		
		if (!(s_name && s_description && s_credential && s_expire))
		{
			fatalError("Missing/invalid parameters in user");
			return;
		}
			
		enum ADC::ACL::Authorization::Credential credential = ADC::ACL::Authorization::convertCredential(s_credential);
		if (credential == ADC::ACL::Authorization::Invalid)
		{
			error("Invalid credentials for user");
		}
		
		Samurai::TimeStamp expire = Samurai::TimeStamp(quickdc_atoi(s_expire));
		if (expire < Samurai::TimeStamp())
		{
			warning("Authorization has expired");
		}
		
		current = new Authorization(credential, expire, s_name, s_description);
		
	}
	else if (!strcasecmp(localName, "id") || !strcasecmp(localName, "verify"))
	{
		if (!current)
		{
			onProblem();
			return;
		}
	
		bool is_id = (!strcasecmp(localName, "id"));
	
		const char* s_type = 0;
		const char* s_data = 0;

		for (size_t n = 0; n < list.size(); n++)
		{
			if (!strcasecmp(list[n].first, "type"))
				s_type = list[n].second;
			else if (!strcasecmp(list[n].first, "data"))
				s_data = list[n].second;
		}
		
		if (!(s_type && s_data))
		{
			fatalError("Missing/invalid parameters in id/verify");
			return;
		}
		
		if (is_id)
		{
			enum ADC::ACL::Identity::IdentityType type = ADC::ACL::Identity::convertIdentityType(s_type);
			if (type == ADC::ACL::Identity::Invalid)
			{
				error("Invalid identity type");
				return;
			}
			
			ADC::ACL::Identity* id = new ADC::ACL::Identity(type, s_data);
			current->addIdentity(id);
		}
		else
		{
			enum ADC::ACL::Verification::VerificationType type = ADC::ACL::Verification::convertVerificationType(s_type);
			if (type == ADC::ACL::Verification::Invalid)
			{
				error("Invalid verification type");
				return;
			}
			ADC::ACL::Verification* vrfy = new ADC::ACL::Verification(type, s_data);
			current->addVerification(vrfy);
		}
	}
}

void ADC::ACL::XMLReader::endElement(const char*, const char* localName, const char*)
{
	if (!strcasecmp(localName, "User"))
	{
		acl->add(current);
		current = 0;
	}
}

void ADC::ACL::XMLReader::characters(const char*, size_t, size_t)
{
}

void ADC::ACL::XMLReader::ignorableWhitespace(const char*, size_t, size_t)
{
	// QDBG("  whitespace,  (@%d, %d)", start, length);
}

void ADC::ACL::XMLReader::warning(const char* str) { QERR("ACL WARNING: %s", str); }
void ADC::ACL::XMLReader::error(const char* str)   { QERR("ACL ERROR:   %s", str); }
void ADC::ACL::XMLReader::fatalError(const char*)  { onProblem(); }

void ADC::ACL::XMLReader::onProblem()
{
	problem = true;
}

/* ------------------------------------------------------------------------- */



