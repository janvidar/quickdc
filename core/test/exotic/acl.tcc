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
	if (!fh) return;
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

/*
 * Loads one ACL document and compares what the table holds against 'expect'.
 *
 * Returns whether it matched, and prints what it got when it did not - which is
 * the detail an assertion on the call alone would not show. Callers wrap it in
 * EXO_ASSERT.
 */
static bool check(const char* label, const char* xml, const char* expect)
{
	const std::string path = aclPath();
	writeFile(path, xml);

	std::string out;
	{
		ADC::ACL::Table table(path.c_str());
		out = render(table);
	}

	/* Before the comparison, so the scratch file goes whatever the outcome. */
	Samurai::IO::File(path).remove();

	if (out == expect) return true;

	printf("    %s\n      expected: %s\n      actual:   %s\n",
		label, expect, out.c_str());
	return false;
}

#define ACL(body) \
	"<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n" \
	"<AccessList version=\"1\" updated=\"1000\">\r\n" body "</AccessList>\r\n"

/* Far enough out that the list does not start expiring mid-suite. */
#define EXPIRE "4102444800"

EXO_TEST(acl_empty_list,
{
	EXO_ASSERT(check("empty list", ACL(""), ""));
	return 1;
});

EXO_TEST(acl_one_user,
{
	EXO_ASSERT(check("one user",
			ACL("<User credential=\"operator\" name=\"op\" description=\"an op\" expire=\"" EXPIRE "\"></User>"),
			"[operator op an op]"));
	return 1;
});

EXO_TEST(acl_several_users,
{
	EXO_ASSERT(check("several users",
			ACL("<User credential=\"admin\" name=\"a\" description=\"d1\" expire=\"" EXPIRE "\"></User>"
			    "<User credential=\"user\" name=\"b\" description=\"d2\" expire=\"" EXPIRE "\"></User>"
			    "<User credential=\"banned\" name=\"c\" description=\"d3\" expire=\"" EXPIRE "\"></User>"),
			"[admin a d1][user b d2][banned c d3]"));
	return 1;
});

/* Self-closing, which is what save() does not write but a hand-edited file may. */
EXO_TEST(acl_self_closing_user,
{
	EXO_ASSERT(check("self-closing user",
			ACL("<User credential=\"guest\" name=\"g\" description=\"d\" expire=\"" EXPIRE "\"/>"),
			"[guest g d]"));
	return 1;
});

EXO_TEST(acl_every_credential,
{
	EXO_ASSERT(check("every credential",
			ACL("<User credential=\"banned\" name=\"1\" description=\"d\" expire=\"" EXPIRE "\"/>"
			    "<User credential=\"guest\" name=\"2\" description=\"d\" expire=\"" EXPIRE "\"/>"
			    "<User credential=\"user\" name=\"3\" description=\"d\" expire=\"" EXPIRE "\"/>"
			    "<User credential=\"link\" name=\"4\" description=\"d\" expire=\"" EXPIRE "\"/>"
			    "<User credential=\"operator\" name=\"5\" description=\"d\" expire=\"" EXPIRE "\"/>"
			    "<User credential=\"admin\" name=\"6\" description=\"d\" expire=\"" EXPIRE "\"/>"),
			"[banned 1 d][guest 2 d][user 3 d][link 4 d][operator 5 d][admin 6 d]"));
	return 1;
});

EXO_TEST(acl_attribute_names_are_matched_without_case,
{
	EXO_ASSERT(check("attribute names are matched without case",
			ACL("<USER CREDENTIAL=\"admin\" NAME=\"a\" DESCRIPTION=\"d\" EXPIRE=\"" EXPIRE "\"/>"),
			"[admin a d]"));
	return 1;
});

EXO_TEST(acl_escaped_name_and_description,
{
	EXO_ASSERT(check("escaped name and description",
			ACL("<User credential=\"user\" name=\"a &amp; b\" description=\"&lt;d&gt;\" expire=\"" EXPIRE "\"/>"),
			"[user a & b <d>]"));
	return 1;
});

EXO_TEST(acl_utf_8_name,
{
	EXO_ASSERT(check("utf-8 name",
			ACL("<User credential=\"user\" name=\"Bj\xc3\xb8rn\" description=\"d\" expire=\"" EXPIRE "\"/>"),
			"[user Bj\xc3\xb8rn d]"));
	return 1;
});

/*
	 * A user missing any of its four attributes is refused outright. The start
	 * tag builds nothing, so the end tag has nothing to hand over - and the
	 * table walks its entries without checking them.
	 */
EXO_TEST(acl_user_with_no_name,
{
	EXO_ASSERT(check("user with no name",
			ACL("<User credential=\"admin\" description=\"d\" expire=\"" EXPIRE "\"></User>"),
			""));
	return 1;
});

EXO_TEST(acl_user_with_no_credential,
{
	EXO_ASSERT(check("user with no credential",
			ACL("<User name=\"a\" description=\"d\" expire=\"" EXPIRE "\"></User>"),
			""));
	return 1;
});

EXO_TEST(acl_user_with_no_description,
{
	EXO_ASSERT(check("user with no description",
			ACL("<User credential=\"admin\" name=\"a\" expire=\"" EXPIRE "\"></User>"),
			""));
	return 1;
});

EXO_TEST(acl_user_with_no_expiry,
{
	EXO_ASSERT(check("user with no expiry",
			ACL("<User credential=\"admin\" name=\"a\" description=\"d\"></User>"),
			""));
	return 1;
});

EXO_TEST(acl_user_with_an_empty_name,
{
	EXO_ASSERT(check("user with an empty name",
			ACL("<User credential=\"admin\" name=\"\" description=\"d\" expire=\"" EXPIRE "\"></User>"),
			""));
	return 1;
});

EXO_TEST(acl_self_closing_user_with_no_name,
{
	EXO_ASSERT(check("self-closing user with no name",
			ACL("<User credential=\"admin\" description=\"d\" expire=\"" EXPIRE "\"/>"),
			""));
	return 1;
});

EXO_TEST(acl_no_attributes_at_all,
{
	EXO_ASSERT(check("no attributes at all",
			ACL("<User></User>"),
			""));
	return 1;
});

/* A refused user must not take the ones around it with it. */
EXO_TEST(acl_a_refused_user_among_good_ones,
{
	EXO_ASSERT(check("a refused user among good ones",
			ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\"/>"
			    "<User credential=\"admin\" description=\"d\" expire=\"" EXPIRE "\"/>"
			    "<User credential=\"user\" name=\"c\" description=\"d\" expire=\"" EXPIRE "\"/>"),
			"[admin a d][user c d]"));
	return 1;
});

/* An unknown credential is reported but the user is still recorded. */
EXO_TEST(acl_unknown_credential,
{
	EXO_ASSERT(check("unknown credential",
			ACL("<User credential=\"wizard\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\"/>"),
			"[invalid a d]"));
	return 1;
});

/* Identities and verifications hang off the user. */
EXO_TEST(acl_user_with_identities,
{
	EXO_ASSERT(check("user with identities",
			ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">"
			    "<id type=\"cid\" data=\"AAAA\"/>"
			    "<id type=\"name\" data=\"nick\"/>"
			    "<verify type=\"password\" data=\"secret\"/>"
			    "</User>"),
			"[admin a d]"));
	return 1;
});

EXO_TEST(acl_identity_with_no_data,
{
	EXO_ASSERT(check("identity with no data",
			ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">"
			    "<id type=\"cid\"/>"
			    "</User>"),
			"[admin a d]"));
	return 1;
});

EXO_TEST(acl_identity_of_an_unknown_type,
{
	EXO_ASSERT(check("identity of an unknown type",
			ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">"
			    "<id type=\"telepathy\" data=\"x\"/>"
			    "</User>"),
			"[admin a d]"));
	return 1;
});

/* An id outside any user must not be attached to anything. */
EXO_TEST(acl_identity_with_no_user,
{
	EXO_ASSERT(check("identity with no user",
			ACL("<id type=\"cid\" data=\"AAAA\"/>"),
			""));
	return 1;
});

/* Malformed structure. None of this may put a null in the table. */
EXO_TEST(acl_nested_users,
{
	EXO_ASSERT(check("nested users",
			ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">"
			    "<User credential=\"user\" name=\"b\" description=\"d\" expire=\"" EXPIRE "\"/>"
			    "</User>"),
			"[admin a d]"));
	return 1;
});

EXO_TEST(acl_end_tag_with_nothing_open,
{
	EXO_ASSERT(check("end tag with nothing open",
			ACL("</User><User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\"/>"),
			"[admin a d]"));
	return 1;
});

EXO_TEST(acl_more_end_tags_than_start_tags,
{
	EXO_ASSERT(check("more end tags than start tags",
			ACL("<User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\"></User></User></User>"),
			"[admin a d]"));
	return 1;
});

EXO_TEST(acl_truncated_in_the_middle_of_a_user,
{
	EXO_ASSERT(check("truncated in the middle of a user",
			"<AccessList><User credential=\"admin\" name=\"a\" description=\"d\" expire=\"" EXPIRE "\">",
			""));
	return 1;
});

EXO_TEST(acl_not_xml_at_all,
{
	EXO_ASSERT(check("not xml at all", "this is not an access list", ""));
	return 1;
});

EXO_TEST(acl_empty_file,
{
	EXO_ASSERT(check("empty file", "", ""));
	return 1;
});

/* A missing file leaves an empty table rather than anything worse. */
EXO_TEST(acl_a_missing_file_leaves_an_empty_table,
{
	ADC::ACL::Table table("/tmp/quickdc_acl_does_not_exist_4711.xml");
	EXO_ASSERT(table.countUsers() == 0);
	EXO_ASSERT(table.getUser(0) == 0);

	Samurai::IO::File("/tmp/quickdc_acl_does_not_exist_4711.xml").remove();
	return 1;
});

/*
 * What save() writes, load() has to read back as the same table. This is the
 * path the hub actually takes: read at start-up, write at shutdown.
 */
EXO_TEST(acl_save_and_load_round_trip,
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

	Samurai::IO::File(path).remove();

	EXO_ASSERT(first == "[operator a & b <d>][banned Bj\xc3\xb8rn d]");
	if (first != second)
	{
		const std::string why = "a save/load round trip changed the table:\n"
			"      first:  " + first + "\n      second: " + second;
		EXO_FAIL(why.c_str());
	}
	return 1;
});
