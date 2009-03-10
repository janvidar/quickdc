/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "xml/writer.h"
#include "xml/node.h"
#include <samurai/io/buffer.h>

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


char* unescape(char* str)
{
	if (!str) return 0;
	for (size_t t = 0; t < XML_TRANSLATIONS; t++) {
		size_t newsize = 0;
		size_t max = strlen(str);
		for (size_t i = 0; i < max; i++) {
			if (strncmp(&str[i], xml_translation_table[t].escaped, xml_translation_table[t].length) == 0)
			{
				str[newsize++] = xml_translation_table[t].unescaped;
				i += xml_translation_table[t].length-1;
			} else {
				str[newsize++] = str[i];
			}
		}
		str[newsize] = '\0';
	}
	return str;
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


