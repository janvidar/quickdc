/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_XML_READER
#define HAVE_QUICKDC_XML_READER

#include <sys/types.h>
#include <string>
#include <utility>
#include <vector>

namespace Samurai {
	namespace IO {
		class Buffer;
	}
}

namespace XML {

namespace SAX {

class ContentHandler {
	public:
		virtual ~ContentHandler() { }
		
		typedef std::vector< std::pair<std::string, std::string> > AttributeList;
		
		virtual void startDocument() = 0;
		virtual void endDocument() = 0;
		
		/**
		 * @param uri the Namespace URI, or 0 if the element has no Namespace URI.
		 * @param localName the local name (without prefix), or 0.
		 * @param qName the qualified XML name (with prefix).
		 */
		virtual void startElement(const char* uri, const char* localName, const char* qName, const AttributeList& list) = 0;
		
		/**
		 * @param uri the Namespace URI, or 0 if the element has no Namespace URI.
		 * @param localName the local name (without prefix), or 0.
		 * @param qName the qualified XML name (with prefix).
		 */
		virtual void endElement(const char* uri, const char* localName, const char* qName) = 0;
		
		/**
		 * @param ch the characters from the XML document
		 * @param start the start position in the array
		 * @param length the number of characters to read from the array.
		 */
		virtual void characters(const char* ch, size_t start, size_t length) = 0;
		
		/**
		 * @param ch the characters from the XML document
		 * @param start the start position in the array
		 * @param length the number of characters to read from the array.
		 */
		virtual void ignorableWhitespace(const char* ch, size_t start, size_t length) = 0;
		
		/**
		 * Event function issued from a warning by the XML parser.
		 * The XML parser will still continue parsing.
		 *
		 * @param str Warning message
		 */
		virtual void warning(const char* str) = 0;
		
		/**
		 * Event function issued from an error detected by the XML parser.
		 * The XML parser will continue parsing.
		 *
		 * @param str Warning message
		 */
		virtual void error(const char* str) = 0;
		
		/**
		 * Event function issued in case of a fatal error detected by the XML parser.
		 * The XML parser will not contune parsing the document, and endDocument() is
		 * not being triggered.
		 *
		 * @param str Warning message
		 */
		virtual void fatalError(const char* str) = 0;
};


class XMLReader {
	public:
		/**
		 * How deeply elements may nest before the document is refused.
		 *
		 * Documents come from peers - a file list is one - and a handler that
		 * follows the nesting recursively runs out of stack long before a
		 * hostile document runs out of open tags. No real document is anywhere
		 * near this deep.
		 */
		static constexpr size_t maxNestingDepth = 128;

		XMLReader(Samurai::IO::Buffer* in, ContentHandler* handler);
		virtual ~XMLReader();

		XMLReader(const XMLReader&) = delete;
		XMLReader& operator=(const XMLReader&) = delete;

	public:
		/**
		 * This will actually parse the XML and return a Document.
		 * NOTE: The Document must be freed.
		 *
		 * A document nested deeper than maxNestingDepth is abandoned where the
		 * limit is reached: fatalError() is reported and neither endDocument()
		 * nor any further event follows.
		 */
		void parse();


	protected:
		Samurai::IO::Buffer* in;
		ContentHandler* handler;
};

} // namespace SAX

} // namespace XML

#endif // HAVE_QUICKDC_XML_READER
