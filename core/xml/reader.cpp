/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "xml/reader.h"
#include "xml/node.h"
#include <samurai/io/buffer.h>
#include <string_view>


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
	                QuotedValue, UnquotedValue, SkipQuoted, Comment, CDataSection,
	                Ignore };
	enum TagState state = CData;

	size_t data_begin  = 0; // position of data start
	size_t attr_start  = 0; // position of the attribute name start
	size_t attr_end    = 0; // position after the attribute name
	size_t val_start   = 0; // position of the attribute value start
	size_t token_start = 0; // position after a comment or CDATA introducer
	size_t name_start  = 0;
	size_t name_end    = 0;

	char q_chr        = 0; // type of quote started
	bool emptyBody    = false;
	bool endTag       = false;

	ContentHandler::AttributeList attributes;

	/* isspace() is only defined for EOF and unsigned char values, and a byte
	   over 127 - which any UTF-8 name or value has - is negative where char is
	   signed. */
	auto isSpace = [](char ch) { return isspace((unsigned char) ch) != 0; };

	auto matches = [&](size_t pos, std::string_view text) {
		if (pos + text.size() > in->size()) return false;
		for (size_t t = 0; t < text.size(); t++)
			if (in->at(pos + t) != text[t]) return false;
		return true;
	};

	auto flushText = [&](size_t end) {
		if (end <= data_begin) return;

		bool wspace = true;
		for (size_t t = data_begin; t < end; t++)
			if (!isSpace(in->at(t)))
				wspace = false;

		/* Nothing white space only can carry an entity, which is worth skipping
		   because indentation is most of a file list's character data. */
		const std::string raw = in->copyRange(data_begin, end);
		if (wspace) {
			handler->ignorableWhitespace(raw.c_str(), 0, raw.size());
		} else {
			const std::string text = XML::unescape(raw);
			handler->characters(text.c_str(), 0, text.size());
		}
	};

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

				flushText(n);

				state = Modifier;
				balance++;
			}
			
		} else if (state == Ignore) {
			if (c == '>') {
				state = CData;
				data_begin = n+1;
			}

		/* A comment ends at "-->", not at the first '>', which may appear
		   inside it. token_start is where its body begins, so the introducer's
		   own dashes cannot be mistaken for the terminator. */
		} else if (state == Comment) {
			if (c == '>' && n >= token_start + 2
				&& in->at(n-1) == '-' && in->at(n-2) == '-') {
				state = CData;
				data_begin = n+1;
			}

		/* CDATA ends at "]]>" and its content is literal - no markup, no
		   entities. */
		} else if (state == CDataSection) {
			if (c == '>' && n >= token_start + 2
				&& in->at(n-1) == ']' && in->at(n-2) == ']') {
				const std::string text = in->copyRange(token_start, n-2);
				if (!text.empty())
					handler->characters(text.c_str(), 0, text.size());
				state = CData;
				data_begin = n+1;
			}

		} else if (state == Modifier) {
			switch (c) {
				case '!': // e.g. comment, CDATA or <!DOCTYPE
					balance--;
					if (matches(n+1, "--")) {
						token_start = n+3;
						state = Comment;
					} else if (matches(n+1, "[CDATA[")) {
						token_start = n+8;
						state = CDataSection;
					} else {
						state = Ignore;
					}
					break;

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
			if (isSpace(c)) {
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
			} else if (!isSpace(c)) {
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
			} else if (isSpace(c)) {
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
			} else if (!isSpace(c)) {
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
			/* A value may itself begin with a '/', so only a '/' that closes
			   the tag means the value is absent. */
			} else if (c == '/' && matches(n+1, ">")) {
				emitAttribute("");
				emptyBody = true;
				state = Open;
			} else if (!isSpace(c)) {
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
			/* A '/' ends the value only where it closes the tag. Anywhere else
			   it is an ordinary character, as in a URL. */
			const bool selfClose = (c == '/' && matches(n+1, ">"));
			if (isSpace(c) || selfClose || c == '>') {
				emitAttribute(in->copyRange(val_start, n));
				if (c == '>') {
					emitTag();
					state = CData;
					data_begin = n+1;
				} else {
					if (selfClose) emptyBody = true;
					state = Open;
				}
			}
		}

	}

	/* Character data is delivered when the next '<' arrives, so whatever
	   trails the last tag needs flushing by hand. */
	if (document && state == CData)
		flushText(in->size());

	if (balance) {
		handler->error("Document is not well formed XML");
		QDBG("Balance: %d", balance);
	}

	if (document)
		handler->endDocument();
}

