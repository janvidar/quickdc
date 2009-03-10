/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_XML_NODE
#define HAVE_QUICKDC_XML_NODE


#include <string>
#include <map>
#include <vector>

namespace Samurai {
	namespace IO {
		class Buffer;
	}
}

namespace XML {

namespace DOM {

class Node;

typedef std::string                          DOMString;
typedef std::vector<Node*>                   NodeList;
typedef std::vector<Node*>::iterator         NodeListIterator;
typedef std::map<DOMString, DOMString>           NamedNodeMap;
typedef std::map<DOMString, DOMString>::iterator NamedNodeMapIterator;

class Exception {
	public:
		enum ExceptionCode {
			INDEX_SIZE_ERR                 = 1,
			DOMSTRING_SIZE_ERR             = 2,
			HIERARCHY_REQUEST_ERR          = 3,
			WRONG_DOCUMENT_ERR             = 4,
			INVALID_CHARACTER_ERR          = 5,
			NO_DATA_ALLOWED_ERR            = 6,
			NO_MODIFICATION_ALLOWED_ERR    = 7,
			NOT_FOUND_ERR                  = 8,
			NOT_SUPPORTED_ERR              = 9,
			INUSE_ATTRIBUTE_ERR            = 10,
			INVALID_STATE_ERR              = 11,
			SYNTAX_ERR                     = 12,
			INVALID_MODIFICATION_ERR       = 13,
			NAMESPACE_ERR                  = 14,
			INVALID_ACCESS_ERR             = 15,
			VALIDATION_ERR                 = 16,
			TYPE_MISMATCH_ERR              = 17
		};
		
		Exception(enum ExceptionCode code) {
			this->code = code;
		}
		
		enum ExceptionCode getExceptionCode() const {
			return code;
		}
		
	protected:
		enum ExceptionCode code;
};


/*
 * This class defines a DOM alike (but not compliant) API.
 */
class Node {
	public:
		Node(Node* parent, const DOMString& name);
		Node(Node* parent, const DOMString& name, const DOMString& value);
		virtual ~Node();

	protected:
		Node();
		
	public:
		Node*             removeChild(Node* oldChild);
		Node*             appendChild(Node* newChild);
		bool              hasChildNodes();
		bool              hasAttributes();
		const DOMString&  getAttribute(const DOMString& name);
		void              setAttribute(const DOMString& name, const DOMString& value);
		void              removeAttribute(const DOMString& name);
		bool              hasAttribute(const DOMString& name);
		
		const DOMString&  getValue() const { return value; }
		const DOMString&  getName() const  { return name; }
		

		void              write(Samurai::IO::Buffer* out);
		
	protected:
		DOMString         name;
		DOMString         value;
		
		Node*             parent;
		NodeList          children;
		NamedNodeMap      attributes;
};


class Document : public Node {
	public:
		Document();
		Document(const DOMString& doctype);
		virtual ~Document();
		
		DOMString getDocType() const { return doctype; }
		void setDocType(const DOMString& doctype);
		bool haveDocType() const;

	protected:
		DOMString doctype;
};

} // namespace DOM
} // namespace XML

#endif // HAVE_QUICKDC_XML_NODE
