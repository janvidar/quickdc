/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_XML_WRITER
#define HAVE_QUICKDC_XML_WRITER

namespace Samurai {
	namespace IO {
		class Buffer;
	}
}

namespace XML {

namespace DOM {
class Node;
class Document;


class XMLWriter {
	public:
		XMLWriter(DOM::Document* doc, Samurai::IO::Buffer* out);
		virtual ~XMLWriter();
		
	public:
		/**
		 * This will actually execute and write the
		 * DOM-tree to the IOBuffer as XML.
		 */
		void exec();
		
	protected:
		Document* doc;
		Samurai::IO::Buffer* out;
};


} // namespace DOM

} // namespace XML

#endif // HAVE_QUICKDC_XML_WRITER
