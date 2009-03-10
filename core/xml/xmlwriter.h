/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_XML_WRITER
#define HAVE_QUICKDC_XML_WRITER

namespace QuickDC {

class IOBuffer;
class Node;



class XMLWriter {
	public:
		XMLWriter(IOBuffer* out);
		virtual ~XMLWriter();
		
	public:
		void addNode(const Node* node);
		
	protected:
		IOBuffer* out;
};


}

#endif // HAVE_QUICKDC_XML_WRITER
