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

/* Nor may an end tag with nothing open buy any extra depth. */
EXO_TEST(xml_stray_end_tags_do_not_buy_extra_depth,
{
	/* TCCIFY: reports failure or a skip its own way. Convert by hand
	   to EXO_FAIL(msg) / EXO_SKIP(reason) - this test cannot fail
	   as it stands. */

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
		else
		{
			printf("FAIL stray end tags do not buy extra depth\n");
			failures++;
		}
	
	return 1;
});
