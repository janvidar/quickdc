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

static int failures = 0;

/*
 * Records every callback as text. Attributes are rendered inside the start tag
 * so an unstripped quote or a lost value shows up in the comparison.
 */
class Recorder : public XML::SAX::ContentHandler
{
	public:
		std::string log;

		void startDocument() { log += "[doc]"; }
		void endDocument() { log += "[/doc]"; }

		void startElement(const char*, const char* localName, const char*, const AttributeList& list)
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

		void endElement(const char*, const char* localName, const char*)
		{
			log += "</";
			log += localName;
			log += ">";
		}

		void characters(const char* ch, size_t start, size_t length)
		{
			log += "'";
			log += std::string(&ch[start], length);
			log += "'";
		}

		void ignorableWhitespace(const char*, size_t, size_t) { }

		void warning(const char* str) { log += "[warn:"; log += str; log += "]"; }
		void error(const char* str) { log += "[err:"; log += str; log += "]"; }
		void fatalError(const char* str) { log += "[fatal:"; log += str; log += "]"; }
};

namespace XML {
extern void escape(Samurai::IO::Buffer* buffer, const char* str);
}

static void check(const char* label, const char* xml, const char* expect)
{
	Samurai::IO::Buffer buf;
	buf.append(xml, strlen(xml));

	Recorder rec;
	XML::SAX::XMLReader reader(&buf, &rec);
	reader.parse();

	if (rec.log == expect)
	{
		printf("  ok %s\n", label);
		return;
	}

	printf("FAIL %s\n    input:    %s\n    expected: %s\n    actual:   %s\n",
		label, xml, expect, rec.log.c_str());
	failures++;
}

/*
 * Whatever the writer escapes the reader must hand back unchanged. This is the
 * property the multi-pass decoder broke: "&lt;" goes out as "&amp;lt;" and came
 * back as "<".
 */
static void checkRoundTrip(const char* payload)
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
	if (rec.log == expect)
	{
		printf("  ok round trip %s\n", payload);
		return;
	}

	printf("FAIL round trip\n    payload:  %s\n    escaped:  %s\n    expected: %s\n    actual:   %s\n",
		payload, body.c_str(), expect.c_str(), rec.log.c_str());
	failures++;
}

/*
 * Nesting, which a peer decides: a file list is parsed with this, and a handler
 * that follows the nesting recursively runs out of stack long before a hostile
 * document runs out of open tags. Anything past the limit is refused where it is
 * reached, so nothing downstream ever sees a tree it cannot walk.
 */
static void checkNesting(const char* label, size_t depth, bool expectRefused)
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

	if (refused == expectRefused && completed == !expectRefused)
	{
		printf("  ok %s\n", label);
		return;
	}

	printf("FAIL %s\n    depth:    %d\n    expected: %s\n    actual:   %s\n",
		label, (int) depth, expectRefused ? "refused" : "accepted", rec.log.c_str());
	failures++;
}

int main()
{
	check("empty document", "", "");

	check("one element",
		"<a></a>",
		"[doc]<a></a>[/doc]");

	check("character data",
		"<a>text</a>",
		"[doc]<a>'text'</a>[/doc]");

	check("nested elements",
		"<a><b>text</b></a>",
		"[doc]<a><b>'text'</b></a>[/doc]");

	check("prolog and comment are skipped",
		"<?xml version=\"1.0\"?><!-- note --><root></root>",
		"[doc]<root></root>[/doc]");

	/* An attribute value keeps neither its quotes nor the delimiter. */
	check("double quoted attribute",
		"<a x=\"1\"></a>",
		"[doc]<a x=1></a>[/doc]");

	check("single quoted attribute",
		"<a x='one'></a>",
		"[doc]<a x=one></a>[/doc]");

	check("several attributes",
		"<a x=\"1\" y=\"2\" z=\"3\"></a>",
		"[doc]<a x=1 y=2 z=3></a>[/doc]");

	check("empty attribute value",
		"<a x=\"\"></a>",
		"[doc]<a x=></a>[/doc]");

	check("entities in an attribute are unescaped",
		"<a x=\"&amp;&lt;&gt;&quot;&apos;\"></a>",
		"[doc]<a x=&<>\"'></a>[/doc]");

	/* A slash inside a quoted value is not a tag terminator. */
	check("slash inside a quoted value",
		"<a href=\"http://example.org/x\"></a>",
		"[doc]<a href=http://example.org/x></a>[/doc]");

	/*
	 * Self-closing tags. The name or the last attribute runs straight into the
	 * '/', which used to be taken as part of it, and the empty body was then
	 * never reported.
	 */
	check("self-closing, no attributes",
		"<a/>",
		"[doc]<a></a>[/doc]");

	check("self-closing after an attribute",
		"<a x=\"1\"/>",
		"[doc]<a x=1></a>[/doc]");

	check("self-closing with a space before the slash",
		"<a x=\"1\" />",
		"[doc]<a x=1></a>[/doc]");

	check("self-closing after a valueless attribute",
		"<a standalone/>",
		"[doc]<a standalone=></a>[/doc]");

	check("self-closing nested in a parent",
		"<a><b x=\"1\"/></a>",
		"[doc]<a><b x=1></b></a>[/doc]");

	/* What a remote file list actually looks like. */
	check("file list",
		"<FileListing Version=\"1\">"
		"<Directory Name=\"music\">"
		"<File Name=\"a.mp3\" Size=\"123\" TTH=\"XYZ\"/>"
		"</Directory>"
		"</FileListing>",
		"[doc]<FileListing Version=1>"
		"<Directory Name=music>"
		"<File Name=a.mp3 Size=123 TTH=XYZ></File>"
		"</Directory>"
		"</FileListing>[/doc]");

	/* Attributes belong to their own tag and do not leak into the next. */
	check("attributes are cleared between tags",
		"<a x=\"1\"></a><b></b>",
		"[doc]<a x=1></a><b></b>[/doc]");

	/*
	 * A quoted run where an attribute name belongs is skipped rather than
	 * swallowing the rest of the document. Both quote characters, which is what
	 * the duplicated test in the Open state used to get wrong.
	 */
	check("stray single quoted run",
		"<a 'foo'></a>",
		"[doc]<a></a>[/doc]");

	check("stray double quoted run",
		"<a \"foo\"></a>",
		"[doc]<a></a>[/doc]");

	check("stray double quoted run before an attribute",
		"<a \"foo\" x=\"1\"></a>",
		"[doc]<a x=1></a>[/doc]");

	/* XML allows white space on either side of the '='. */
	check("space before the equals",
		"<a x =\"1\"></a>",
		"[doc]<a x=1></a>[/doc]");

	check("space after the equals",
		"<a x= \"1\"></a>",
		"[doc]<a x=1></a>[/doc]");

	check("space on both sides of the equals",
		"<a x = \"1\"></a>",
		"[doc]<a x=1></a>[/doc]");

	check("spaced equals among ordinary attributes",
		"<a w=\"0\" x = \"1\" y=\"2\"></a>",
		"[doc]<a w=0 x=1 y=2></a>[/doc]");

	check("spaced equals before a self-closing slash",
		"<a x = \"1\"/>",
		"[doc]<a x=1></a>[/doc]");

	check("newline around the equals",
		"<a x\n=\n\"1\"></a>",
		"[doc]<a x=1></a>[/doc]");

	/* A valueless attribute still ends at the space, not at the next name. */
	check("valueless attribute followed by another",
		"<a x y=\"1\"></a>",
		"[doc]<a x= y=1></a>[/doc]");

	check("valueless attribute before the close",
		"<a x ></a>",
		"[doc]<a x=></a>[/doc]");

	/* Unquoted values are not XML, but the parser has always taken them. */
	check("unquoted value",
		"<a x=1></a>",
		"[doc]<a x=1></a>[/doc]");

	check("unquoted value then another attribute",
		"<a x=1 y=2></a>",
		"[doc]<a x=1 y=2></a>[/doc]");

	/* Inside a value the delimiters are ordinary characters. */
	check("angle bracket inside a value",
		"<a x=\"p>q\"></a>",
		"[doc]<a x=p>q></a>[/doc]");

	check("the other quote inside a value",
		"<a x=\"it's\"></a>",
		"[doc]<a x=it's></a>[/doc]");

	/* A '/' in an unquoted value is data unless it closes the tag. */
	check("unquoted url value",
		"<a href=http://example.org/x></a>",
		"[doc]<a href=http://example.org/x></a>[/doc]");

	check("unquoted url value, self-closing",
		"<a href=http://example.org/x/>",
		"[doc]<a href=http://example.org/x></a>[/doc]");

	check("unquoted value containing a slash",
		"<a x=p/q></a>",
		"[doc]<a x=p/q></a>[/doc]");

	check("unquoted value beginning with a slash",
		"<a x=/root></a>",
		"[doc]<a x=/root></a>[/doc]");

	check("unquoted value absent before a self-closing slash",
		"<a x=/>",
		"[doc]<a x=></a>[/doc]");

	/* Character data is unescaped, as attribute values already were. */
	check("entities in character data",
		"<a>&lt;b&gt; &amp; &quot;c&quot;</a>",
		"[doc]<a>'<b> & \"c\"'</a>[/doc]");

	check("text after the last tag is not lost",
		"<a></a>tail",
		"[doc]<a></a>'tail'[/doc]");

	/* A comment ends at "-->", so a '>' inside it is not a terminator. */
	check("comment containing an angle bracket",
		"<a><!-- p > q --><b></b></a>",
		"[doc]<a><b></b></a>[/doc]");

	check("empty comment",
		"<a><!----></a>",
		"[doc]<a></a>[/doc]");

	check("comment containing a tag",
		"<a><!-- <b></b> --></a>",
		"[doc]<a></a>[/doc]");

	/* CDATA is literal: no markup, no entities. */
	check("cdata section",
		"<a><![CDATA[p>q & <b>]]></a>",
		"[doc]<a>'p>q & <b>'</a>[/doc]");

	check("empty cdata section",
		"<a><![CDATA[]]></a>",
		"[doc]<a></a>[/doc]");

	/* A doctype has no special terminator and still ends at its '>'. */
	check("doctype is skipped",
		"<!DOCTYPE a><a></a>",
		"[doc]<a></a>[/doc]");

	/* Bytes over 127 must not be handed to isspace() as a negative char. */
	check("utf-8 in an attribute value",
		"<a x=\"\xc3\xa6\xc3\xb8\xc3\xa5\"></a>",
		"[doc]<a x=\xc3\xa6\xc3\xb8\xc3\xa5></a>[/doc]");

	check("utf-8 in character data",
		"<a>\xc3\xa6\xc3\xb8\xc3\xa5</a>",
		"[doc]<a>'\xc3\xa6\xc3\xb8\xc3\xa5'</a>[/doc]");

	/* Numeric character references, decimal and hexadecimal, encoded as UTF-8. */
	check("decimal character reference",
		"<a>&#65;&#66;</a>",
		"[doc]<a>'AB'</a>[/doc]");

	check("hex character reference",
		"<a>&#x41;&#X42;</a>",
		"[doc]<a>'AB'</a>[/doc]");

	check("character reference in an attribute",
		"<a x=\"&#65;\"></a>",
		"[doc]<a x=A></a>[/doc]");

	/* Two bytes: U+00E5. */
	check("two byte character reference",
		"<a>&#229;</a>",
		"[doc]<a>'\xc3\xa5'</a>[/doc]");

	/* Three bytes: U+263A. */
	check("three byte character reference",
		"<a>&#x263A;</a>",
		"[doc]<a>'\xe2\x98\xba'</a>[/doc]");

	/* Four bytes: U+1F600. */
	check("four byte character reference",
		"<a>&#x1F600;</a>",
		"[doc]<a>'\xf0\x9f\x98\x80'</a>[/doc]");

	check("character reference beside text",
		"<a>a&#66;c</a>",
		"[doc]<a>'aBc'</a>[/doc]");

	/* Anything that is not a reference we understand is left as it stands. */
	check("bare ampersand",
		"<a>a &amp; b</a>",
		"[doc]<a>'a & b'</a>[/doc]");

	check("unknown entity is left alone",
		"<a>&nbsp;</a>",
		"[doc]<a>'&nbsp;'</a>[/doc]");

	check("empty reference is left alone",
		"<a>&#;</a>",
		"[doc]<a>'&#;'</a>[/doc]");

	check("non-digit reference is left alone",
		"<a>&#xZZ;</a>",
		"[doc]<a>'&#xZZ;'</a>[/doc]");

	check("out of range reference is left alone",
		"<a>&#x110000;</a>",
		"[doc]<a>'&#x110000;'</a>[/doc]");

	check("surrogate reference is left alone",
		"<a>&#xD800;</a>",
		"[doc]<a>'&#xD800;'</a>[/doc]");

	check("nul reference is left alone",
		"<a>&#0;</a>",
		"[doc]<a>'&#0;'</a>[/doc]");

	check("unterminated reference is left alone",
		"<a>&#65</a>",
		"[doc]<a>'&#65'</a>[/doc]");

	/*
	 * What a reference expands to is not scanned again, so an escaped ampersand
	 * followed by an entity name is that text and not the entity.
	 */
	check("expansion is not rescanned",
		"<a x=\"&amp;lt;\">&amp;gt;</a>",
		"[doc]<a x=&lt;>'&gt;'</a>[/doc]");

	/* A CDATA section is literal, so a reference inside it stays as written. */
	check("reference inside cdata is literal",
		"<a><![CDATA[&#65;&amp;]]></a>",
		"[doc]<a>'&#65;&amp;'</a>[/doc]");

	checkNesting("nesting at the limit is accepted",
		XML::SAX::XMLReader::maxNestingDepth, false);

	checkNesting("nesting one past the limit is refused",
		XML::SAX::XMLReader::maxNestingDepth + 1, true);

	checkNesting("deeply nested document is refused",
		XML::SAX::XMLReader::maxNestingDepth * 100, true);

	/* Depth is what is bounded, not how many elements there are. */
	{
		std::string doc;
		for (size_t n = 0; n < XML::SAX::XMLReader::maxNestingDepth * 10; n++)
			doc += "<a/>";

		std::string expect = "[doc]";
		for (size_t n = 0; n < XML::SAX::XMLReader::maxNestingDepth * 10; n++)
			expect += "<a></a>";
		expect += "[/doc]";

		check("many siblings are not nesting", doc.c_str(), expect.c_str());
	}

	/* Nor may an end tag with nothing open buy any extra depth. */
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

		if (rec.log.find("[fatal:") == std::string::npos)
			printf("  ok stray end tags do not buy extra depth\n");
		else
		{
			printf("FAIL stray end tags do not buy extra depth\n");
			failures++;
		}
	}

	checkRoundTrip("&");
	checkRoundTrip("<");
	checkRoundTrip(">");
	checkRoundTrip("\"");
	checkRoundTrip("'");
	checkRoundTrip("&lt;");
	checkRoundTrip("&amp;");
	checkRoundTrip("a&b<c>d\"e'f");
	checkRoundTrip("Bj\xc3\xb8rn & S\xc3\xb8n <2024>");

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
