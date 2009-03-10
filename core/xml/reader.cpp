/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "xml/reader.h"
#include "xml/node.h"
#include <samurai/io/buffer.h>


namespace XML {
extern char* unescape(char* str);
}

const char xml_white_space_table[5] = { ' ', '\t', '\n', '\r' };

XML::SAX::XMLReader::XMLReader(Samurai::IO::Buffer* in_, ContentHandler* handler_) : in(in_), handler(handler_) {
	
}

XML::SAX::XMLReader::~XMLReader() {

}

#define EXTRACT_TAG() \
{ \
	char* tagname = in->memdup(name_start, name_end); \
	if (!endTag) { \
		handler->startElement(0, tagname, 0, attributes); \
		if (emptyBody) \
			handler->endElement(0, tagname, 0); \
	} else { \
		handler->endElement(0, tagname, 0); \
	} \
	free(tagname); \
	balance--; \
}

#define EXTRACT_ATTRIBUTE() \
{ \
	if (isAttrib) { \
		char* attr_name = 0; \
		char* attr_val = 0; \
		if (attr_split > attr_start) { \
			attr_name = in->memdup(attr_start, attr_split); \
			if ((in->at(attr_split+1) == '\"' && in->at(n-1) == '\"') || \
				(in->at(attr_split+1) == '\'' && in->at(n-1) == '\'')) { \
				attr_val = in->memdup(attr_split+2, n-1); \
			} else { \
				attr_val = in->memdup(attr_split+1, n); \
			} \
		} else { \
			attr_name = in->memdup(attr_start, n); \
		} \
		if (!strlen(attr_name)) { \
			handler->error("Invalid empty attribute name"); \
		} \
		attributes.push_back(std::pair<char*,char*>(XML::unescape(attr_name), XML::unescape(attr_val))); \
		isAttrib = false; \
	} \
}


void XML::SAX::XMLReader::parse() {
	if (!handler) return;
	
	bool document = false;
	int balance = 0;
	
	enum TagState { CData, Modifier, Name, Open, Attrib, Quote, Ignore };
	enum TagState state = CData;
	
	size_t data_begin = 0; // position of data start
	size_t tag_begin  = 0; // position of '<'
	size_t attr_start = 0; // position of attribute start
	size_t attr_split = 0; // position of attribute split (=)
	size_t q_start    = 0; // position of quote start
	size_t name_start = 0;
	size_t name_end   = 0;
	
	char q_chr        = 0; // type of quote started
	bool emptyBody    = false;
	bool endTag       = false;
	bool isAttrib     = false;
	
	ContentHandler::AttributeList attributes;
	
	for (size_t n = 0; n < in->size(); n++) {
		char c = (char) in->at(n);
		if (state == CData) {
			if (c == '<') {
				if (!document) {
					document = true;
					handler->startDocument();
				}
			
				while (attributes.size()) {
					free((char*) attributes.back().first);
					free((char*) attributes.back().second);
					attributes.pop_back();
				}
			
				// start of tag.
				tag_begin = n;
				emptyBody = false;
				endTag    = false;
				// FIXME: Character data callback
				
				// Check if this is only white space only
				if (n > data_begin) {
					bool wspace = true;
					for (size_t t = data_begin; t < n; t++)
						if (!isspace(in->at(t)))
							wspace = false;
	
					char* p = in->memdup(data_begin, n);
					if (wspace)
						handler->ignorableWhitespace(p, 0, n - data_begin);
					else
						handler->characters(p, 0, n - data_begin);
					free(p);
				}
				
				state = Modifier;
				balance++;
			}
			
		} else if (state == Ignore) {
			if (c == '>') {
				state = CData;
				data_begin = n+1;
			}
			
		} else if (state == Modifier) {
			switch (c) {
				case '!': // e.g. comment
				case '?': // e.g. <?xml ...
					balance--;
					state = Ignore;
					break; // ignored! :-)
				case '>':
					handler->error("Empty tag entity");
					state = CData;
					data_begin = n+1;
					break;
					
				case '/':
					endTag = true;
					break;
					
				default:
					name_start = n;
					state = Name;
			}
		
		} else if (state == Name) {
			if (isspace(c)) {
				if (endTag) handler->error("Whitespace in end-tag");
				name_end = n;
				state = Open;
			} else if (c == '>') {
				name_end = n;
				EXTRACT_TAG();
				data_begin = n+1;
				state = CData;
			}
		
		} else if (state == Open) {
			switch (c) {
				case '>':
				{
					EXTRACT_TAG();
					state = CData;
					data_begin = n+1;
					break;
				}
				case '/':
					emptyBody = true;
					break;
					
				default:
					if (!isspace(c)) {
						if (c == '\'' || c == '\'') {
							q_chr = c;
							q_start = n;
							attr_start = n;
							state = Quote;
						} else {
							state = Attrib;
							attr_start = n;
							isAttrib = true;
						}
					}
			}
		
		} else if (state == Attrib) {
			switch (c) {
				case '>':
				{
					EXTRACT_ATTRIBUTE();
					EXTRACT_TAG();
					state = CData;
					data_begin = n+1;
					break;
				}
				case '=':
					attr_split = n;
					break;
					
				case '\"':
				case '\'':
						q_chr = c;
						q_start = n;
						state = Quote;
						break;
				default:
					if (isspace(c)) {
						EXTRACT_ATTRIBUTE();
						state = Open;
					}
				}
		} else if (state == Quote) {
			if ((c == '\"' || c == '\'') && q_chr == c) {
				state = Attrib;
			}
		}
		
	}
	
	if (balance) {
		handler->error("Document is not well formed XML");
		QDBG("Balance: %d", balance);
	}
	
	if (document)
		handler->endDocument();
}

