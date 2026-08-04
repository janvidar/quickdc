/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

/*
 * Drives the SAX reader over a document and compares the whole callback
 * sequence against an expected string, so an event that goes missing is as
 * visible as one that carries the wrong text.
 *
 * The self-closing cases are the point of this: QuickDC's own writer emits
 * "<File ... />" with a space before the slash, so a round trip through it
 * never reaches the state the other clients' "/>" does.
 */

#include "quickdc.h"
#include "xml/reader.h"

#include <samurai/io/buffer.h>

#include <stdio.h>
#include <string.h>
#include <string>

/*
 * Records every callback as text. Attributes are rendered inside the start tag
 * so an unstripped quote or a lost value shows up in the comparison.
 */
class Recorder : public XML::SAX::ContentHandler
{
	public:
		std::string log;

		void startDocument() override { log += "[doc]"; }
		void endDocument() override { log += "[/doc]"; }

		void startElement(const char*, const char* localName, const char*, const AttributeList& list) override
		{
			log += "<";
			log += localName;
			for (size_t n = 0; n < list.size(); n++)
			{
				log += " ";
				log += list[n].first;
				log += "=";
				log += list[n].second;
			}
			log += ">";
		}

		void endElement(const char*, const char* localName, const char*) override
		{
			log += "</";
			log += localName;
			log += ">";
		}

		void characters(const char* ch, size_t start, size_t length) override
		{
			log += "'";
			log += std::string(&ch[start], length);
			log += "'";
		}

		void ignorableWhitespace(const char*, size_t, size_t) override { }

		void warning(const char* str) override { log += "[warn:"; log += str; log += "]"; }
		void error(const char* str) override { log += "[err:"; log += str; log += "]"; }
		void fatalError(const char* str) override { log += "[fatal:"; log += str; log += "]"; }
};

namespace XML {
extern void escape(Samurai::IO::Buffer* buffer, const char* str);
}

static bool check(const char* label, const char* xml, const char* expect)
{
	Samurai::IO::Buffer buf;
	buf.append(xml, strlen(xml));

	Recorder rec;
	XML::SAX::XMLReader reader(&buf, &rec);
	reader.parse();

	if (rec.log == expect) return true;

	printf("FAIL %s\n    input:    %s\n    expected: %s\n    actual:   %s\n",
		label, xml, expect, rec.log.c_str());
	return false;
}

/*
 * Whatever the writer escapes the reader must hand back unchanged. This is the
 * property the multi-pass decoder broke: "&lt;" goes out as "&amp;lt;" and came
 * back as "<".
 */
static bool checkRoundTrip(const char* payload)
{
	Samurai::IO::Buffer escaped;
	XML::escape(&escaped, payload);
	const std::string body = escaped.copyRange(0, escaped.size());

	Samurai::IO::Buffer doc;
	doc.append("<a x=\"", 6);
	doc.append(body.c_str(), body.size());
	doc.append("\"></a>", 6);

	Recorder rec;
	XML::SAX::XMLReader reader(&doc, &rec);
	reader.parse();

	const std::string expect = std::string("[doc]<a x=") + payload + "></a>[/doc]";
	if (rec.log == expect) return true;

	printf("FAIL round trip\n    payload:  %s\n    escaped:  %s\n    expected: %s\n    actual:   %s\n",
		payload, body.c_str(), expect.c_str(), rec.log.c_str());
	return false;
}

/*
 * Nesting, which a peer decides: a file list is parsed with this, and a handler
 * that follows the nesting recursively runs out of stack long before a hostile
 * document runs out of open tags. Anything past the limit is refused where it is
 * reached, so nothing downstream ever sees a tree it cannot walk.
 */
static bool checkNesting(const char* label, size_t depth, bool expectRefused)
{
	std::string doc;
	for (size_t n = 0; n < depth; n++) doc += "<a>";
	for (size_t n = 0; n < depth; n++) doc += "</a>";

	Samurai::IO::Buffer buf;
	buf.append(doc.c_str(), doc.size());

	Recorder rec;
	XML::SAX::XMLReader reader(&buf, &rec);
	reader.parse();

	const bool refused = rec.log.find("[fatal:") != std::string::npos;

	/* A refused document stops where it was: no endDocument() follows it. */
	const bool completed = rec.log.find("[/doc]") != std::string::npos;

	if (refused == expectRefused && completed == !expectRefused) return true;

	printf("FAIL %s\n    depth:    %d\n    expected: %s\n    actual:   %s\n",
		label, (int) depth, expectRefused ? "refused" : "accepted", rec.log.c_str());
	return false;
}

EXO_TEST(xml_empty_document,
{
	EXO_ASSERT(check("empty document", "", ""));
	return 1;
});

EXO_TEST(xml_one_element,
{
	EXO_ASSERT(check("one element",
			"<a></a>",
			"[doc]<a></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_character_data,
{
	EXO_ASSERT(check("character data",
			"<a>text</a>",
			"[doc]<a>'text'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_nested_elements,
{
	EXO_ASSERT(check("nested elements",
			"<a><b>text</b></a>",
			"[doc]<a><b>'text'</b></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_prolog_and_comment_are_skipped,
{
	EXO_ASSERT(check("prolog and comment are skipped",
			"<?xml version=\"1.0\"?><!-- note --><root></root>",
			"[doc]<root></root>[/doc]"));
	return 1;
});

/* An attribute value keeps neither its quotes nor the delimiter. */
EXO_TEST(xml_double_quoted_attribute,
{
	EXO_ASSERT(check("double quoted attribute",
			"<a x=\"1\"></a>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_single_quoted_attribute,
{
	EXO_ASSERT(check("single quoted attribute",
			"<a x='one'></a>",
			"[doc]<a x=one></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_several_attributes,
{
	EXO_ASSERT(check("several attributes",
			"<a x=\"1\" y=\"2\" z=\"3\"></a>",
			"[doc]<a x=1 y=2 z=3></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_empty_attribute_value,
{
	EXO_ASSERT(check("empty attribute value",
			"<a x=\"\"></a>",
			"[doc]<a x=></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_entities_in_an_attribute_are_unescaped,
{
	EXO_ASSERT(check("entities in an attribute are unescaped",
			"<a x=\"&amp;&lt;&gt;&quot;&apos;\"></a>",
			"[doc]<a x=&<>\"'></a>[/doc]"));
	return 1;
});

/* A slash inside a quoted value is not a tag terminator. */
EXO_TEST(xml_slash_inside_a_quoted_value,
{
	EXO_ASSERT(check("slash inside a quoted value",
			"<a href=\"http://example.org/x\"></a>",
			"[doc]<a href=http://example.org/x></a>[/doc]"));
	return 1;
});

/*
	 * Self-closing tags. The name or the last attribute runs straight into the
	 * '/', which used to be taken as part of it, and the empty body was then
	 * never reported.
	 */
EXO_TEST(xml_self_closing_no_attributes,
{
	EXO_ASSERT(check("self-closing, no attributes",
			"<a/>",
			"[doc]<a></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_self_closing_after_an_attribute,
{
	EXO_ASSERT(check("self-closing after an attribute",
			"<a x=\"1\"/>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_self_closing_with_a_space_before_the_slash,
{
	EXO_ASSERT(check("self-closing with a space before the slash",
			"<a x=\"1\" />",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_self_closing_after_a_valueless_attribute,
{
	EXO_ASSERT(check("self-closing after a valueless attribute",
			"<a standalone/>",
			"[doc]<a standalone=></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_self_closing_nested_in_a_parent,
{
	EXO_ASSERT(check("self-closing nested in a parent",
			"<a><b x=\"1\"/></a>",
			"[doc]<a><b x=1></b></a>[/doc]"));
	return 1;
});

/* What a remote file list actually looks like. */
EXO_TEST(xml_file_list,
{
	EXO_ASSERT(check("file list",
			"<FileListing Version=\"1\">"
			"<Directory Name=\"music\">"
			"<File Name=\"a.mp3\" Size=\"123\" TTH=\"XYZ\"/>"
			"</Directory>"
			"</FileListing>",
			"[doc]<FileListing Version=1>"
			"<Directory Name=music>"
			"<File Name=a.mp3 Size=123 TTH=XYZ></File>"
			"</Directory>"
			"</FileListing>[/doc]"));
	return 1;
});

/* Attributes belong to their own tag and do not leak into the next. */
EXO_TEST(xml_attributes_are_cleared_between_tags,
{
	EXO_ASSERT(check("attributes are cleared between tags",
			"<a x=\"1\"></a><b></b>",
			"[doc]<a x=1></a><b></b>[/doc]"));
	return 1;
});

/*
	 * A quoted run where an attribute name belongs is skipped rather than
	 * swallowing the rest of the document. Both quote characters, which is what
	 * the duplicated test in the Open state used to get wrong.
	 */
EXO_TEST(xml_stray_single_quoted_run,
{
	EXO_ASSERT(check("stray single quoted run",
			"<a 'foo'></a>",
			"[doc]<a></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_stray_double_quoted_run,
{
	EXO_ASSERT(check("stray double quoted run",
			"<a \"foo\"></a>",
			"[doc]<a></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_stray_double_quoted_run_before_an_attribute,
{
	EXO_ASSERT(check("stray double quoted run before an attribute",
			"<a \"foo\" x=\"1\"></a>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

/* XML allows white space on either side of the '='. */
EXO_TEST(xml_space_before_the_equals,
{
	EXO_ASSERT(check("space before the equals",
			"<a x =\"1\"></a>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_space_after_the_equals,
{
	EXO_ASSERT(check("space after the equals",
			"<a x= \"1\"></a>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_space_on_both_sides_of_the_equals,
{
	EXO_ASSERT(check("space on both sides of the equals",
			"<a x = \"1\"></a>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_spaced_equals_among_ordinary_attributes,
{
	EXO_ASSERT(check("spaced equals among ordinary attributes",
			"<a w=\"0\" x = \"1\" y=\"2\"></a>",
			"[doc]<a w=0 x=1 y=2></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_spaced_equals_before_a_self_closing_slash,
{
	EXO_ASSERT(check("spaced equals before a self-closing slash",
			"<a x = \"1\"/>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_newline_around_the_equals,
{
	EXO_ASSERT(check("newline around the equals",
			"<a x\n=\n\"1\"></a>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

/* A valueless attribute still ends at the space, not at the next name. */
EXO_TEST(xml_valueless_attribute_followed_by_another,
{
	EXO_ASSERT(check("valueless attribute followed by another",
			"<a x y=\"1\"></a>",
			"[doc]<a x= y=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_valueless_attribute_before_the_close,
{
	EXO_ASSERT(check("valueless attribute before the close",
			"<a x ></a>",
			"[doc]<a x=></a>[/doc]"));
	return 1;
});

/* Unquoted values are not XML, but the parser has always taken them. */
EXO_TEST(xml_unquoted_value,
{
	EXO_ASSERT(check("unquoted value",
			"<a x=1></a>",
			"[doc]<a x=1></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_unquoted_value_then_another_attribute,
{
	EXO_ASSERT(check("unquoted value then another attribute",
			"<a x=1 y=2></a>",
			"[doc]<a x=1 y=2></a>[/doc]"));
	return 1;
});

/* Inside a value the delimiters are ordinary characters. */
EXO_TEST(xml_angle_bracket_inside_a_value,
{
	EXO_ASSERT(check("angle bracket inside a value",
			"<a x=\"p>q\"></a>",
			"[doc]<a x=p>q></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_the_other_quote_inside_a_value,
{
	EXO_ASSERT(check("the other quote inside a value",
			"<a x=\"it's\"></a>",
			"[doc]<a x=it's></a>[/doc]"));
	return 1;
});

/* A '/' in an unquoted value is data unless it closes the tag. */
EXO_TEST(xml_unquoted_url_value,
{
	EXO_ASSERT(check("unquoted url value",
			"<a href=http://example.org/x></a>",
			"[doc]<a href=http://example.org/x></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_unquoted_url_value_self_closing,
{
	EXO_ASSERT(check("unquoted url value, self-closing",
			"<a href=http://example.org/x/>",
			"[doc]<a href=http://example.org/x></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_unquoted_value_containing_a_slash,
{
	EXO_ASSERT(check("unquoted value containing a slash",
			"<a x=p/q></a>",
			"[doc]<a x=p/q></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_unquoted_value_beginning_with_a_slash,
{
	EXO_ASSERT(check("unquoted value beginning with a slash",
			"<a x=/root></a>",
			"[doc]<a x=/root></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_unquoted_value_absent_before_a_self_closing_slash,
{
	EXO_ASSERT(check("unquoted value absent before a self-closing slash",
			"<a x=/>",
			"[doc]<a x=></a>[/doc]"));
	return 1;
});

/* Character data is unescaped, as attribute values already were. */
EXO_TEST(xml_entities_in_character_data,
{
	EXO_ASSERT(check("entities in character data",
			"<a>&lt;b&gt; &amp; &quot;c&quot;</a>",
			"[doc]<a>'<b> & \"c\"'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_text_after_the_last_tag_is_not_lost,
{
	EXO_ASSERT(check("text after the last tag is not lost",
			"<a></a>tail",
			"[doc]<a></a>'tail'[/doc]"));
	return 1;
});

/* A comment ends at "-->", so a '>' inside it is not a terminator. */
EXO_TEST(xml_comment_containing_an_angle_bracket,
{
	EXO_ASSERT(check("comment containing an angle bracket",
			"<a><!-- p > q --><b></b></a>",
			"[doc]<a><b></b></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_empty_comment,
{
	EXO_ASSERT(check("empty comment",
			"<a><!----></a>",
			"[doc]<a></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_comment_containing_a_tag,
{
	EXO_ASSERT(check("comment containing a tag",
			"<a><!-- <b></b> --></a>",
			"[doc]<a></a>[/doc]"));
	return 1;
});

/* CDATA is literal: no markup, no entities. */
EXO_TEST(xml_cdata_section,
{
	EXO_ASSERT(check("cdata section",
			"<a><![CDATA[p>q & <b>]]></a>",
			"[doc]<a>'p>q & <b>'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_empty_cdata_section,
{
	EXO_ASSERT(check("empty cdata section",
			"<a><![CDATA[]]></a>",
			"[doc]<a></a>[/doc]"));
	return 1;
});

/* A doctype has no special terminator and still ends at its '>'. */
EXO_TEST(xml_doctype_is_skipped,
{
	EXO_ASSERT(check("doctype is skipped",
			"<!DOCTYPE a><a></a>",
			"[doc]<a></a>[/doc]"));
	return 1;
});

/* Bytes over 127 must not be handed to isspace() as a negative char. */
EXO_TEST(xml_utf_8_in_an_attribute_value,
{
	EXO_ASSERT(check("utf-8 in an attribute value",
			"<a x=\"\xc3\xa6\xc3\xb8\xc3\xa5\"></a>",
			"[doc]<a x=\xc3\xa6\xc3\xb8\xc3\xa5></a>[/doc]"));
	return 1;
});

EXO_TEST(xml_utf_8_in_character_data,
{
	EXO_ASSERT(check("utf-8 in character data",
			"<a>\xc3\xa6\xc3\xb8\xc3\xa5</a>",
			"[doc]<a>'\xc3\xa6\xc3\xb8\xc3\xa5'</a>[/doc]"));
	return 1;
});

/* Numeric character references, decimal and hexadecimal, encoded as UTF-8. */
EXO_TEST(xml_decimal_character_reference,
{
	EXO_ASSERT(check("decimal character reference",
			"<a>&#65;&#66;</a>",
			"[doc]<a>'AB'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_hex_character_reference,
{
	EXO_ASSERT(check("hex character reference",
			"<a>&#x41;&#X42;</a>",
			"[doc]<a>'AB'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_character_reference_in_an_attribute,
{
	EXO_ASSERT(check("character reference in an attribute",
			"<a x=\"&#65;\"></a>",
			"[doc]<a x=A></a>[/doc]"));
	return 1;
});

/* Two bytes: U+00E5. */
EXO_TEST(xml_two_byte_character_reference,
{
	EXO_ASSERT(check("two byte character reference",
			"<a>&#229;</a>",
			"[doc]<a>'\xc3\xa5'</a>[/doc]"));
	return 1;
});

/* Three bytes: U+263A. */
EXO_TEST(xml_three_byte_character_reference,
{
	EXO_ASSERT(check("three byte character reference",
			"<a>&#x263A;</a>",
			"[doc]<a>'\xe2\x98\xba'</a>[/doc]"));
	return 1;
});

/* Four bytes: U+1F600. */
EXO_TEST(xml_four_byte_character_reference,
{
	EXO_ASSERT(check("four byte character reference",
			"<a>&#x1F600;</a>",
			"[doc]<a>'\xf0\x9f\x98\x80'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_character_reference_beside_text,
{
	EXO_ASSERT(check("character reference beside text",
			"<a>a&#66;c</a>",
			"[doc]<a>'aBc'</a>[/doc]"));
	return 1;
});

/* Anything that is not a reference we understand is left as it stands. */
EXO_TEST(xml_bare_ampersand,
{
	EXO_ASSERT(check("bare ampersand",
			"<a>a &amp; b</a>",
			"[doc]<a>'a & b'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_unknown_entity_is_left_alone,
{
	EXO_ASSERT(check("unknown entity is left alone",
			"<a>&nbsp;</a>",
			"[doc]<a>'&nbsp;'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_empty_reference_is_left_alone,
{
	EXO_ASSERT(check("empty reference is left alone",
			"<a>&#;</a>",
			"[doc]<a>'&#;'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_non_digit_reference_is_left_alone,
{
	EXO_ASSERT(check("non-digit reference is left alone",
			"<a>&#xZZ;</a>",
			"[doc]<a>'&#xZZ;'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_out_of_range_reference_is_left_alone,
{
	EXO_ASSERT(check("out of range reference is left alone",
			"<a>&#x110000;</a>",
			"[doc]<a>'&#x110000;'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_surrogate_reference_is_left_alone,
{
	EXO_ASSERT(check("surrogate reference is left alone",
			"<a>&#xD800;</a>",
			"[doc]<a>'&#xD800;'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_nul_reference_is_left_alone,
{
	EXO_ASSERT(check("nul reference is left alone",
			"<a>&#0;</a>",
			"[doc]<a>'&#0;'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_unterminated_reference_is_left_alone,
{
	EXO_ASSERT(check("unterminated reference is left alone",
			"<a>&#65</a>",
			"[doc]<a>'&#65'</a>[/doc]"));
	return 1;
});

/*
	 * What a reference expands to is not scanned again, so an escaped ampersand
	 * followed by an entity name is that text and not the entity.
	 */
EXO_TEST(xml_expansion_is_not_rescanned,
{
	EXO_ASSERT(check("expansion is not rescanned",
			"<a x=\"&amp;lt;\">&amp;gt;</a>",
			"[doc]<a x=&lt;>'&gt;'</a>[/doc]"));
	return 1;
});

/* A CDATA section is literal, so a reference inside it stays as written. */
EXO_TEST(xml_reference_inside_cdata_is_literal,
{
	EXO_ASSERT(check("reference inside cdata is literal",
			"<a><![CDATA[&#65;&amp;]]></a>",
			"[doc]<a>'&#65;&amp;'</a>[/doc]"));
	return 1;
});

EXO_TEST(xml_nesting_at_the_limit_is_accepted,
{
	EXO_ASSERT(checkNesting("nesting at the limit is accepted",
			XML::SAX::XMLReader::maxNestingDepth, false));
	return 1;
});

EXO_TEST(xml_nesting_one_past_the_limit_is_refused,
{
	EXO_ASSERT(checkNesting("nesting one past the limit is refused",
			XML::SAX::XMLReader::maxNestingDepth + 1, true));
	return 1;
});

EXO_TEST(xml_deeply_nested_document_is_refused,
{
	EXO_ASSERT(checkNesting("deeply nested document is refused",
			XML::SAX::XMLReader::maxNestingDepth * 100, true));
	return 1;
});

EXO_TEST(xml_,
{
	EXO_ASSERT(checkRoundTrip("&"));
	return 1;
});

EXO_TEST(xml__2,
{
	EXO_ASSERT(checkRoundTrip("<"));
	return 1;
});

EXO_TEST(xml__2_2,
{
	EXO_ASSERT(checkRoundTrip(">"));
	return 1;
});

EXO_TEST(xml__2_2_2,
{
	EXO_ASSERT(checkRoundTrip("\""));
	return 1;
});

EXO_TEST(xml__2_2_2_2,
{
	EXO_ASSERT(checkRoundTrip("'"));
	return 1;
});

EXO_TEST(xml_lt,
{
	EXO_ASSERT(checkRoundTrip("&lt;"));
	return 1;
});

EXO_TEST(xml_amp,
{
	EXO_ASSERT(checkRoundTrip("&amp;"));
	return 1;
});

EXO_TEST(xml_a_b_c_d_e_f,
{
	EXO_ASSERT(checkRoundTrip("a&b<c>d\"e'f"));
	return 1;
});

EXO_TEST(xml_bj_xc3_xb8rn_s_xc3_xb8n_2024,
{
	EXO_ASSERT(checkRoundTrip("Bj\xc3\xb8rn & S\xc3\xb8n <2024>"));
	return 1;
});

/* Nor may an end tag with nothing open buy any extra depth. */
EXO_TEST(xml_stray_end_tags_do_not_buy_extra_depth,
{
		std::string doc;
		for (size_t n = 0; n < 200; n++) doc += "</a>";
		for (size_t n = 0; n < XML::SAX::XMLReader::maxNestingDepth; n++) doc += "<a>";
		for (size_t n = 0; n < XML::SAX::XMLReader::maxNestingDepth; n++) doc += "</a>";

		Samurai::IO::Buffer buf;
		buf.append(doc.c_str(), doc.size());

		Recorder rec;
		XML::SAX::XMLReader reader(&buf, &rec);
		reader.parse();

		/*
		 * No fatal report: the two hundred stray end tags must not have counted
		 * towards the nesting limit, so the document that follows them still fits.
		 */
		EXO_ASSERT(rec.log.find("[fatal:") == std::string::npos);
	
	return 1;
});
