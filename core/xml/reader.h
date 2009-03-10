/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_XML_READER
#define HAVE_QUICKDC_XML_READER

#include <sys/types.h>
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
		
		typedef std::vector< std::pair<const char*, const char*> > AttributeList;
		
		virtual void startDocument() = 0;
		virtual void endDocument() = 0;
		
		/**
		 * @param uri the Namespace URI, or 0 if the element has no Namespace URI.
		 * @param localName the local name (without prefix), or 0.
		 * @param qName the qualified XML name (with prefix).
		 */
		virtual void startElement(const char* uri, const char* localName, const char* qName, AttributeList list) = 0;
		
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
		XMLReader(Samurai::IO::Buffer* in, ContentHandler* handler);
		virtual ~XMLReader();
		
	public:
		/**
		 * This will actually parse the XML and return a Document.
		 * NOTE: The Document must be freed.
		 */
		void parse();
		

	protected:
		Samurai::IO::Buffer* in;
		ContentHandler* handler;
};

} // namespace SAX

} // namespace XML

#endif // HAVE_QUICKDC_XML_READER
