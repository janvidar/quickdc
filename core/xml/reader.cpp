/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "xml/reader.h"
#include "xml/node.h"
#include <samurai/io/buffer.h>


namespace XML {
extern std::string unescape(std::string str);
}

XML::SAX::XMLReader::XMLReader(Samurai::IO::Buffer* in_, ContentHandler* handler_) : in(in_), handler(handler_) {
	
}

XML::SAX::XMLReader::~XMLReader() {

}

void XML::SAX::XMLReader::parse() {
	if (!handler) return;

	bool document = false;
	int balance = 0;

	/*
	 * A name and its value are separate runs rather than one run split at the
	 * '=', because XML allows white space on either side of it.
	 */
	enum TagState { CData, Modifier, Name, Open, AttrName, AfterName, AfterEquals,
	                QuotedValue, UnquotedValue, SkipQuoted, Ignore };
	enum TagState state = CData;

	size_t data_begin = 0; // position of data start
	size_t attr_start = 0; // position of the attribute name start
	size_t attr_end   = 0; // position after the attribute name
	size_t val_start  = 0; // position of the attribute value start
	size_t name_start = 0;
	size_t name_end   = 0;

	char q_chr        = 0; // type of quote started
	bool emptyBody    = false;
	bool endTag       = false;

	ContentHandler::AttributeList attributes;

	auto emitTag = [&]() {
		const std::string tagname = in->copyRange(name_start, name_end);
		if (!endTag) {
			handler->startElement(0, tagname.c_str(), 0, attributes);
			if (emptyBody)
				handler->endElement(0, tagname.c_str(), 0);
		} else {
			handler->endElement(0, tagname.c_str(), 0);
		}
		balance--;
	};

	auto emitAttribute = [&](const std::string& value) {
		const std::string name = in->copyRange(attr_start, attr_end);
		if (name.empty())
			handler->error("Invalid empty attribute name");
		attributes.emplace_back(XML::unescape(name), XML::unescape(value));
	};

	for (size_t n = 0; n < in->size(); n++) {
		char c = (char) in->at(n);
		if (state == CData) {
			if (c == '<') {
				if (!document) {
					document = true;
					handler->startDocument();
				}
			
				attributes.clear();

				// start of tag.
				emptyBody = false;
				endTag    = false;
				// FIXME: Character data callback
				
				// Check if this is only white space only
				if (n > data_begin) {
					bool wspace = true;
					for (size_t t = data_begin; t < n; t++)
						if (!isspace(in->at(t)))
							wspace = false;
	
					const std::string p = in->copyRange(data_begin, n);
					if (wspace)
						handler->ignorableWhitespace(p.c_str(), 0, n - data_begin);
					else
						handler->characters(p.c_str(), 0, n - data_begin);
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
			} else if (c == '/') {
				name_end = n;
				emptyBody = true;
				state = Open;
			} else if (c == '>') {
				name_end = n;
				emitTag();
				data_begin = n+1;
				state = CData;
			}

		} else if (state == Open) {
			if (c == '>') {
				emitTag();
				state = CData;
				data_begin = n+1;
			} else if (c == '/') {
				emptyBody = true;
			} else if (c == '\"' || c == '\'') {
				/* A quoted run where a name belongs is not an attribute; skip
				   it rather than read the rest of the document as quoted. */
				q_chr = c;
				state = SkipQuoted;
			} else if (!isspace(c)) {
				attr_start = n;
				state = AttrName;
			}

		} else if (state == SkipQuoted) {
			if (c == q_chr)
				state = Open;

		} else if (state == AttrName) {
			if (c == '=') {
				attr_end = n;
				state = AfterEquals;
			} else if (isspace(c)) {
				attr_end = n;
				state = AfterName;
			} else if (c == '/' || c == '>') {
				attr_end = n;
				emitAttribute("");
				if (c == '>') {
					emitTag();
					state = CData;
					data_begin = n+1;
				} else {
					emptyBody = true;
					state = Open;
				}
			}

		/* White space between the name and its '=', so the name is complete but
		   it is not yet known whether a value follows. */
		} else if (state == AfterName) {
			if (c == '=') {
				state = AfterEquals;
			} else if (c == '>') {
				emitAttribute("");
				emitTag();
				state = CData;
				data_begin = n+1;
			} else if (c == '/') {
				emitAttribute("");
				emptyBody = true;
				state = Open;
			} else if (!isspace(c)) {
				emitAttribute("");
				attr_start = n;
				state = AttrName;
			}

		/* White space between the '=' and the value. */
		} else if (state == AfterEquals) {
			if (c == '\"' || c == '\'') {
				q_chr = c;
				val_start = n+1;
				state = QuotedValue;
			} else if (c == '>') {
				emitAttribute("");
				emitTag();
				state = CData;
				data_begin = n+1;
			} else if (c == '/') {
				emitAttribute("");
				emptyBody = true;
				state = Open;
			} else if (!isspace(c)) {
				val_start = n;
				state = UnquotedValue;
			}

		} else if (state == QuotedValue) {
			/* Only the matching quote ends it, so '>' and '/' are literal. */
			if (c == q_chr) {
				emitAttribute(in->copyRange(val_start, n));
				state = Open;
			}

		} else if (state == UnquotedValue) {
			if (isspace(c) || c == '/' || c == '>') {
				emitAttribute(in->copyRange(val_start, n));
				if (c == '>') {
					emitTag();
					state = CData;
					data_begin = n+1;
				} else {
					if (c == '/') emptyBody = true;
					state = Open;
				}
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

