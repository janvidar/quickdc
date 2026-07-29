/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Drives ADC::ACL::XMLReader over hub access lists and checks the table that
 * comes out.
 *
 * The table decides who may log in and with what rights, so an entry that
 * decodes wrongly is worse than one that is refused. Every malformed case here
 * has to leave the table with nothing in it rather than something approximate.
 *
 * Table reads and writes its own file, so each case gets a path of its own
 * under the scratch directory - never the real ~/.quickdc/hub/acl.xml.
 */

#include "quickdc.h"
#include "network/adc/server/acl.h"

#include <samurai/io/file.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int failures = 0;

#define CHECK(x) do { if (!(x)) { \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static std::string aclPath()
{
	static int counter = 0;
	return "/tmp/quickdc_acl_test_" + std::to_string(counter++) + ".xml";
}

static void writeFile(const std::string& path, const char* content)
{
	FILE* fh = fopen(path.c_str(), "wb");
	if (!fh) { printf("FAIL cannot write %s\n", path.c_str()); failures++; return; }
	fwrite(content, 1, strlen(content), fh);
	fclose(fh);
}

/*
 * Renders the loaded table so a whole access list can be asserted at once,
 * including the users that should not have survived.
 */
static std::string render(const ADC::ACL::Table& table)
{
	std::string out;
	for (size_t n = 0; n < table.countUsers(); n++)
	{
		const ADC::ACL::Authorization* auth = table.getUser(n);
		if (!auth) { out += "(null)"; continue; }

		out += "[";
		out += ADC::ACL::Authorization::convertCredential(auth->getCredential());
		out += " ";
		out += auth->getName() ? auth->getName() : "-";
		out += " ";
		out += auth->getDescription() ? auth->getDescription() : "-";
		out += "]";
	}
	return out;
}

static void check(const char* label, const char* xml, const char* expect)
{
	const std::string path = aclPath();
	writeFile(path, xml);

	std::string out;
	{
		ADC::ACL::Table table(path.c_str());
		out = render(table);
	}

	if (out == expect)
		printf("  ok %s\n", label);
	else
	{
		printf("FAIL %s\n    expected: %s\n    actual:   %s\n", label, expect, out.c_str());
		failures++;
	}

	Samurai::IO::File(path).remove();
}

#define ACL(body) \
	"<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n" \
	"<AccessList version=\"1\" updated=\"1000\">\r\n" body "</AccessList>\r\n"

/* Far enough out that the list does not start expiring mid-suite. */
#define EXPIRE "4102444800"

int main()
{
	check("empty list", ACL(""), "");

	check("one user",
		ACL("<User credential=\"operator\" name=\"op\" description=\"an op\" expire=\"" EXPIRE "\"></User>"),
		"[operator op an op]");

	check("several users",
		ACL("<User credential=\"admin\" name=\"a\" description=\"d1\" expire=\"" EXPIRE "\"></User>"
		    "<User credential=\"user\" name=\"b\" description=\"d2\" expire=\"" EXPIRE "\"></User>"
		    "<User credential=\"banned\" name=\"c\" description=\"d3\" expire=\"" EXPIRE "\"></User>"),
		"[admin a d1][user b d2][banned c d3]");

	/* Self-closing, which is what save() does not write but a hand-edited file may. */
	check("self-closing user",
		ACL("<User credential=\"guest\" name=\"g\" description=\"d\" expire=\"" EXPIRE "\"/>"),
		"[guest g d]");

	check("every credential",
		ACL("<User credential=\"banned\" name=\"1\" description=\"d\" expire=\"" EXPIRE "\"/>"
		    "<User credential=\"guest\" name=\"2\" description=\"d\" expire=\"" EXPIRE "\"/>"
		    "<User credential=\"user\" name=\"3\" description=\"d\" expire=\"" EXPIRE "\"/>"
		    "<User credential=\"link\" name=\"4\" description=\"d\" expire=\"" EXPIRE "\"/>"
		    "<User credential=\"operator\" name=\"5\" description=\"d\" expire=\"" EXPIRE "\"/>"
		    "<User credential=\"admin\" name=\"6\" description=\"d\" expire=\"" EXPIRE "\"/>"),
		"[banned 1 d][guest 2 d][user 3 d][link 4 d][operator 5 d][admin 6 d]");

	check("attribute names are matched without case",
		ACL("<USER CREDENTIAL=\"admin\" NAME=\"a\" DESCRIPTION=\"d\" EXPIRE=\"" EXPIRE "\"/>"),
		"[admin a d]");

	check("escaped name and description",
		ACL("<User credential=\"user\" name=\"a &amp; b\" description=\"&lt;d&gt;\" expire=\"" EXPIRE "\"/>"),
		"[user a & b <d>]");

	check("utf-8 name",
		ACL("<User credential=\"user\" name=\"Bj\xc3\xb8rn\" description=\"d\" expire=\"" EXPIRE "\"/>"),
		"[user Bj\xc3\xb8rn d]");

	/*
	 * A user missing any of its four attributes is refused outright. The start
	 * tag builds nothing, so the end tag has nothing to hand over - and the
	 * table walks its entries without checking them.
	 */
	check("user with no name",
		ACL("<User credential=\"admin\" description=\"d\" expire=\"" EXPIRE "\"></User>"),
		"");

	check("user with no credential",
		ACL("<User name=\"a\" description=\"d\" expire=\"" EXPIRE "\"></User>"),
		"");

	check("user with no description",
		ACL("<User credential=\"admin\" name=\"a\" expire=\"" EXPIRE "\"></User>"),
		"");

	check("user with no expiry",
		ACL("<User credential=\"admin\" name=\"a\" description=\"d\"></User>"),
		"");

	check("user with an empty name",
		ACL("<User credential=\"admin\" name=\"\" description=\"d\" expire=\"" EXPIRE "\"></User>"),
		"");

	check("self-closing user with no name",
		ACL("<User credential=\"admin\" description=\"d\" expire=\"" EXPIRE "\"/>"),
		"");

	check("no attributes at all",
		ACL("<User></User>"),
		"");

	/* A refused user must not take the ones around it with it. */
	check("a refused user among good ones",
		ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\"/>"
		    "<User credential=\"admin\" description=\"d\" expire=\"" EXPIRE "\"/>"
		    "<User credential=\"user\" name=\"c\" description=\"d\" expire=\"" EXPIRE "\"/>"),
		"[admin a d][user c d]");

	/* An unknown credential is reported but the user is still recorded. */
	check("unknown credential",
		ACL("<User credential=\"wizard\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\"/>"),
		"[invalid a d]");

	/* Identities and verifications hang off the user. */
	check("user with identities",
		ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">"
		    "<id type=\"cid\" data=\"AAAA\"/>"
		    "<id type=\"name\" data=\"nick\"/>"
		    "<verify type=\"password\" data=\"secret\"/>"
		    "</User>"),
		"[admin a d]");

	check("identity with no data",
		ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">"
		    "<id type=\"cid\"/>"
		    "</User>"),
		"[admin a d]");

	check("identity of an unknown type",
		ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">"
		    "<id type=\"telepathy\" data=\"x\"/>"
		    "</User>"),
		"[admin a d]");

	/* An id outside any user must not be attached to anything. */
	check("identity with no user",
		ACL("<id type=\"cid\" data=\"AAAA\"/>"),
		"");

	/* Malformed structure. None of this may put a null in the table. */
	check("nested users",
		ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">"
		    "<User credential=\"user\" name=\"b\" description=\"d\" expire=\"" EXPIRE "\"/>"
		    "</User>"),
		"[admin a d]");

	check("end tag with nothing open",
		ACL("</User><User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\"/>"),
		"[admin a d]");

	check("more end tags than start tags",
		ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\"></User></User></User>"),
		"[admin a d]");

	check("truncated in the middle of a user",
		"<AccessList><User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">",
		"");

	check("not xml at all", "this is not an access list", "");

	check("empty file", "", "");

	/* A missing file leaves an empty table rather than anything worse. */
	{
		ADC::ACL::Table table("/tmp/quickdc_acl_does_not_exist_4711.xml");
		CHECK(table.countUsers() == 0);
		CHECK(table.getUser(0) == 0);
		printf("  ok missing file\n");
		Samurai::IO::File("/tmp/quickdc_acl_does_not_exist_4711.xml").remove();
	}

	/*
	 * What save() writes, load() has to read back as the same table. This is
	 * the path the hub actually takes: read at start-up, write at shutdown.
	 */
	{
		const std::string path = aclPath();
		writeFile(path,
			ACL("<User credential=\"operator\" name=\"a &amp; b\" description=\"&lt;d&gt;\" expire=\"" EXPIRE "\">"
			    "<id type=\"cid\" data=\"AAAA\"/>"
			    "<verify type=\"password\" data=\"p&amp;w\"/>"
			    "</User>"
			    "<User credential=\"banned\" name=\"Bj\xc3\xb8rn\" description=\"d\" expire=\"" EXPIRE "\"/>"));

		std::string first;
		{
			ADC::ACL::Table table(path.c_str());
			first = render(table);
		}

		/* The destructor above wrote the table back over the same path. */
		std::string second;
		{
			ADC::ACL::Table table(path.c_str());
			second = render(table);
		}

		CHECK(first == "[operator a & b <d>][banned Bj\xc3\xb8rn d]");
		if (first != second)
		{
			printf("FAIL save/load round trip\n    first:  %s\n    second: %s\n",
				first.c_str(), second.c_str());
			failures++;
		}
		else
		{
			printf("  ok save and load round trip\n");
		}

		Samurai::IO::File(path).remove();
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
