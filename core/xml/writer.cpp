/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "xml/writer.h"
#include "xml/node.h"
#include <samurai/io/buffer.h>
#include <cstdint>
#include <string>

#define XML_TRANSLATIONS 5

namespace XML {

static const struct {
		const char*  escaped;
		const char   unescaped;
		const size_t length;
} xml_translation_table[XML_TRANSLATIONS] = {
        { "&amp;",  '&',  5 },
        { "&lt;",   '<',  4 },
        { "&gt;",   '>',  4 },
        { "&quot;", '\"', 6 },
        { "&apos;", '\'', 6 }
};


void escape(Samurai::IO::Buffer* buffer, const char* str) {
	for (size_t t = 0; t < strlen(str); t++) {
		bool handled = false;
		for (size_t l = 0; l < XML_TRANSLATIONS; l++)
		{
			if (str[t] == xml_translation_table[l].unescaped)
			{
				buffer->append(xml_translation_table[l].escaped);
				handled = true;
			}
		}
		if (!handled)
			buffer->append(str[t]);
	}
}

void escape(Samurai::IO::Buffer* buffer, const std::string& str)
{
	for (size_t t = 0; t < str.size(); t++) {
		bool handled = false;
		for (size_t l = 0; l < XML_TRANSLATIONS; l++)
		{
			if (str[t] == xml_translation_table[l].unescaped)
			{
				buffer->append(xml_translation_table[l].escaped);
				handled = true;
			}
		}
		if (!handled)
			buffer->append(str[t]);
	}
}


/* The longest reference worth looking for is "&#x10FFFF;". */
#define XML_REFERENCE_MAX 10

static int digitValue(char c, unsigned base)
{
	unsigned v;
	if (c >= '0' && c <= '9') v = static_cast<unsigned>(c - '0');
	else if (c >= 'a' && c <= 'f') v = static_cast<unsigned>(c - 'a') + 10;
	else if (c >= 'A' && c <= 'F') v = static_cast<unsigned>(c - 'A') + 10;
	else return -1;
	return (v < base) ? static_cast<int>(v) : -1;
}

/* XML 1.0 section 2.2: everything else is not a character a document may name. */
static bool isCharacter(uint32_t cp)
{
	if (cp == 0x9 || cp == 0xa || cp == 0xd) return true;
	if (cp >= 0x20 && cp <= 0xd7ff) return true;
	if (cp >= 0xe000 && cp <= 0xfffd) return true;
	if (cp >= 0x10000 && cp <= 0x10ffff) return true;
	return false;
}

static void appendUTF8(std::string& out, uint32_t cp)
{
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xc0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xe0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
		out += static_cast<char>(0x80 | (cp & 0x3f));
	} else {
		out += static_cast<char>(0xf0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
		out += static_cast<char>(0x80 | (cp & 0x3f));
	}
}

/*
 * Decodes "&#65;" and "&#x41;" as well as the five named entities, in a single
 * left to right pass so that what a reference expands to is never scanned
 * again: "&amp;lt;" is the text "&lt;", not '<'. Anything that is not a
 * reference this understands is copied through as it stands.
 */
std::string unescape(std::string str)
{
	std::string out;
	out.reserve(str.size());

	for (size_t i = 0; i < str.size(); ) {
		if (str[i] != '&') { out += str[i++]; continue; }

		size_t end = std::string::npos;
		const size_t limit = (str.size() < i + XML_REFERENCE_MAX + 1)
			? str.size() : i + XML_REFERENCE_MAX + 1;
		for (size_t s = i + 1; s < limit; s++)
			if (str[s] == ';') { end = s; break; }

		bool decoded = false;

		if (end != std::string::npos && str[i+1] == '#') {
			size_t d = i + 2;
			unsigned base = 10;
			if (d < end && (str[d] == 'x' || str[d] == 'X')) { base = 16; d++; }

			uint32_t cp = 0;
			bool ok = (d < end);
			for (; d < end && ok; d++) {
				const int v = digitValue(str[d], base);
				if (v < 0 || cp > 0x10ffff) ok = false;
				else cp = cp * base + static_cast<uint32_t>(v);
			}

			if (ok && isCharacter(cp)) {
				appendUTF8(out, cp);
				decoded = true;
			}

		} else if (end != std::string::npos) {
			const size_t len = end - i + 1;
			for (size_t t = 0; t < XML_TRANSLATIONS; t++) {
				if (len == xml_translation_table[t].length
					&& str.compare(i, len, xml_translation_table[t].escaped) == 0) {
					out += xml_translation_table[t].unescaped;
					decoded = true;
					break;
				}
			}
		}

		if (decoded) i = end + 1;
		else out += str[i++];
	}

	return out;
}

}

XML::DOM::XMLWriter::XMLWriter(XML::DOM::Document* doc_, Samurai::IO::Buffer* out_) : doc(doc_), out(out_) {
	
}

XML::DOM::XMLWriter::~XMLWriter() {

}



/**
 * This will actually execute and write the
 * DOM-tree to the IOBuffer as XML.
 */
void XML::DOM::XMLWriter::exec() {
	if (!doc) return;
	out->append("<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>");
	if (doc->haveDocType()) {
		out->append("<!DOCTYPE ");
		out->append(doc->getDocType());
		out->append(">");
	}
	doc->write(out);
}


