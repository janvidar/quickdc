/*
 * Copyright (C) 2001-2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * What is left of QuickDC's own XML: the writer, and the small tree the writer
 * walks.
 *
 * Reading is samurai's - Samurai::IO::XmlReader replaced the reader that used to
 * live here, and the cases that covered it went with it, their inputs kept as a
 * corpus in samurai's own suite. What cannot be tested there is the property that
 * spans both: whatever this writer escapes, that reader has to hand back
 * unchanged. That is the one this file exists for now.
 */

#include "quickdc.h"
#include "xml/node.h"

#include <samurai/io/buffer.h>
#include <samurai/io/xml.h>

#include <new>
#include <stdio.h>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>

namespace XML {
extern void escape(Samurai::IO::Buffer* buffer, const char* str);
}

namespace {

/* Records what the reader reports, rendered so a lost value shows up. */
class Recorder final : public Samurai::IO::XmlContentHandler
{
	public:
		std::string log;

		void startElement(std::string_view, std::string_view name,
			const Attributes& attributes) override
		{
			log += "<";
			log += name;
			for (const auto& [attribute, value] : attributes)
			{
				log += " ";
				log += attribute;
				log += "=";
				log += value;
			}
			log += ">";
		}

		void endElement(std::string_view, std::string_view name) override
		{
			log += "</";
			log += name;
			log += ">";
		}

		void characters(std::string_view text) override
		{
			log += "'";
			log += text;
			log += "'";
		}
};

/*
 * Whatever the writer escapes the reader must hand back unchanged.
 *
 * This is the property a multi-pass decoder broke: "&lt;" goes out as "&amp;lt;"
 * and came back as "<", so a filename containing an escape read as the thing it
 * was escaping. The payload goes through an attribute because that is where the
 * writer puts a name.
 */
bool roundTrips(const char* payload)
{
	Samurai::IO::Buffer escaped;
	XML::escape(&escaped, payload);
	const std::string body = escaped.copyRange(0, escaped.size());

	const std::string document = "<a x=\"" + body + "\"></a>";

	Recorder rec;
	const Samurai::IO::XmlError parsed =
		Samurai::IO::XmlReader::parse(document, rec);

	const std::string expect = std::string("<a x=") + payload + "></a>";

	if (parsed == Samurai::IO::XmlError::Ok && rec.log == expect) return true;

	printf("FAIL round trip\n    payload:  %s\n    escaped:  %s\n"
		"    expected: %s\n    actual:   %s (%s)\n",
		payload, body.c_str(), expect.c_str(), rec.log.c_str(),
		Samurai::IO::toString(parsed));
	return false;
}

}

/* ------------------------------------------------------------------------- */
/* What the writer escapes, the reader gives back                            */
/* ------------------------------------------------------------------------- */

EXO_TEST(xml_an_ampersand_survives_being_escaped,
{
	EXO_ASSERT(roundTrips("&"));
	return 1;
});

EXO_TEST(xml_a_less_than_survives_being_escaped,
{
	EXO_ASSERT(roundTrips("<"));
	return 1;
});

EXO_TEST(xml_a_greater_than_survives_being_escaped,
{
	EXO_ASSERT(roundTrips(">"));
	return 1;
});

EXO_TEST(xml_a_double_quote_survives_being_escaped,
{
	EXO_ASSERT(roundTrips("\""));
	return 1;
});

EXO_TEST(xml_a_single_quote_survives_being_escaped,
{
	EXO_ASSERT(roundTrips("'"));
	return 1;
});

/* The two that a decoder rescanning its own output gets wrong. */
EXO_TEST(xml_text_that_looks_like_an_escape_survives_being_escaped,
{
	EXO_ASSERT(roundTrips("&lt;"));
	EXO_ASSERT(roundTrips("&amp;"));
	EXO_ASSERT(roundTrips("&#65;"));
	return 1;
});

EXO_TEST(xml_every_reserved_character_at_once_survives,
{
	EXO_ASSERT(roundTrips("a&b<c>d\"e'f"));
	return 1;
});

/* A filename is not ASCII, and escaping must not touch what is already UTF-8. */
EXO_TEST(xml_utf8_survives_being_escaped,
{
	EXO_ASSERT(roundTrips("Bj\xc3\xb8rn & S\xc3\xb8n <2024>"));
	return 1;
});

/* ------------------------------------------------------------------------- */
/* The DOM tree                                                              */
/* ------------------------------------------------------------------------- */

/*
 * removeChild() answers with the child it removed. It used to read that answer
 * back out of the list *after* erasing from it, which for a vector means the
 * entry has moved: the answer was the next child, or - when the child removed was
 * the last one - a read off the end. The destructor takes children off the back,
 * so the off-the-end case was the ordinary one.
 */
EXO_TEST(xml_removing_a_child_answers_with_that_child,
{
	XML::DOM::Node root(0, "root");

	XML::DOM::Node* first = new XML::DOM::Node(&root, "first");
	XML::DOM::Node* second = new XML::DOM::Node(&root, "second");
	XML::DOM::Node* third = new XML::DOM::Node(&root, "third");

	/* The middle one, where the wrong answer was the one after it. */
	EXO_ASSERT_EQ_PTR(root.removeChild(second), second);

	/* The last one, where the wrong answer was off the end of the vector. */
	EXO_ASSERT_EQ_PTR(root.removeChild(third), third);

	/* And the only one left. */
	EXO_ASSERT_EQ_PTR(root.removeChild(first), first);
	EXO_ASSERT(!root.hasChildNodes());

	delete first;
	delete second;
	delete third;
	return 1;
});

/*
 * A removed child no longer has a parent, so deleting it does not ask its former
 * parent to remove it again - which would find nothing and throw, from inside a
 * destructor.
 */
EXO_TEST(xml_a_removed_child_can_then_be_deleted,
{
	XML::DOM::Node root(0, "root");
	XML::DOM::Node* child = new XML::DOM::Node(&root, "child");

	EXO_ASSERT(root.hasChildNodes());
	EXO_ASSERT_EQ_PTR(root.removeChild(child), child);
	EXO_ASSERT(!root.hasChildNodes());

	/* Threw NOT_FOUND_ERR out of ~Node() before it was detached here. */
	delete child;

	EXO_ASSERT(!root.hasChildNodes());
	return 1;
});

/* Removing something that is not a child is an error rather than an answer. */
EXO_TEST(xml_removing_a_node_that_is_not_a_child_is_refused,
{
	XML::DOM::Node root(0, "root");
	XML::DOM::Node other(0, "other");

	bool threw = false;
	try { root.removeChild(&other); }
	catch (const XML::DOM::Exception&) { threw = true; }

	EXO_ASSERT(threw);
	return 1;
});

/*
 * A tree tears itself down: the destructor takes each child off the back and
 * deletes it, and each child asks to be removed on the way out. That is the path
 * the off-the-end read was on, so it is walked here with children of children.
 */
EXO_TEST(xml_a_tree_of_children_is_torn_down_without_reading_past_the_end,
{
	XML::DOM::Node* root = new XML::DOM::Node(0, "root");

	for (int n = 0; n < 8; n++)
	{
		XML::DOM::Node* branch = new XML::DOM::Node(root, "branch");
		for (int m = 0; m < 4; m++)
			new XML::DOM::Node(branch, "leaf");
	}

	EXO_ASSERT(root->hasChildNodes());
	delete root;
	return 1;
});

/*
 * A node built without a parent records that it has none.
 *
 * The constructors set the parent only inside the branch that had one, so a root
 * node's parent member was never written - and ~Node() asks whatever it holds to
 * remove this child from itself. Whether that crashes depends on what happened to
 * be in the memory, which is why this puts something there: constructed into
 * storage filled with 0xAA, an unset parent is a wild pointer rather than a
 * convenient zero.
 */
EXO_TEST(xml_a_node_built_without_a_parent_says_so,
{
	alignas(XML::DOM::Node) unsigned char storage[sizeof(XML::DOM::Node)];
	memset(storage, 0xAA, sizeof(storage));

	XML::DOM::Node* root = new (storage) XML::DOM::Node(0, "root");
	EXO_ASSERT(root->getName() == "root");
	EXO_ASSERT(!root->hasChildNodes());

	/* A child, so the destructor walks the removal path on the way out. */
	new XML::DOM::Node(root, "child");
	EXO_ASSERT(root->hasChildNodes());

	/* Dereferenced 0xAAAA... when the parent was left unwritten. */
	root->~Node();
	return 1;
});

/* The same for the three-argument constructor, which had it too. */
EXO_TEST(xml_a_valued_node_built_without_a_parent_says_so,
{
	alignas(XML::DOM::Node) unsigned char storage[sizeof(XML::DOM::Node)];
	memset(storage, 0xAA, sizeof(storage));

	XML::DOM::Node* root = new (storage) XML::DOM::Node(0, "root", "value");
	EXO_ASSERT(root->getName() == "root");
	EXO_ASSERT(root->getValue() == "value");

	new XML::DOM::Node(root, "child", "leaf");
	root->~Node();
	return 1;
});
