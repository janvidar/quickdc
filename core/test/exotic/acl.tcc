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

/* A missing file leaves an empty table rather than anything worse. */
EXO_TEST(acl_missing_file,
{
		ADC::ACL::Table table("/tmp/quickdc_acl_does_not_exist_4711.xml");
		EXO_ASSERT(table.countUsers() == 0);
		EXO_ASSERT(table.getUser(0) == 0);
		Samurai::IO::File("/tmp/quickdc_acl_does_not_exist_4711.xml").remove();
	
	return 1;
});

/*
	 * What save() writes, load() has to read back as the same table. This is
	 * the path the hub actually takes: read at start-up, write at shutdown.
	 */
EXO_TEST(acl_save_and_load_round_trip,
{
	/* TCCIFY: reports failure or a skip its own way. Convert by hand
	   to EXO_FAIL(msg) / EXO_SKIP(reason) - this test cannot fail
	   as it stands. */

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

		EXO_ASSERT(first == "[operator a & b <d>][banned Bj\xc3\xb8rn d]");
		if (first != second)
		{
			printf("FAIL save/load round trip\n    first:  %s\n    second: %s\n",
				first.c_str(), second.c_str());
			failures++;
		}
		else
		{
		}

		Samurai::IO::File(path).remove();
	
	return 1;
});
