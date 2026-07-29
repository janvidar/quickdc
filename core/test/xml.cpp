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

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
