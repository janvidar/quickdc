/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_XML_PARSER
#define HAVE_QUICKDC_XML_PARSER

#include <string>
#include <vector>
#include <map>

namespace QuickDC {

#warning Make sure wchar_t is 16 bits!

typedef unsigned long long DOMTimeStamp;
typedef std::string<wchar_t> DOMString;
typedef std::vector<Node*> NodeList;
typedef std::map<DOMString, Node*> NamedNodeMap;
typedef void* DOMUserData;

class DOMObject { };

class DOMStringList {
	public:
		DOMString item(size_t index);
		bool contains(const DOMString& str);
	protected:
		size_t length;
};

class NameList {
	public:
	
		DOMString getName(size_t index);
		DOMString getNamespaceURI(size_t index);
		bool contains(const DOMString str);
		bool containsNS(const DOMString& namespaceURI, const DOMString& name);
	
	protected:
		size_t length;
};


class DOMImplementationList {
	public:
		DOMImplementation* item(size_t index);
		
	protected:
		size_t length;
};

// Introduced in DOM Level 3:
class DOMImplementationSource {
	public:
		DOMImplementation* getDOMImplementation(const DOMString& features);
		DOMImplementationList* getDOMImplementationList(const DOMString& features);
};

class DOMException {
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
		
		ExceptionCode code;
};

class Node {
	
	public:
		enum NodeType {
			ELEMENT_NODE                   = 1,
			ATTRIBUTE_NODE                 = 2,
			TEXT_NODE                      = 3,
			CDATA_SECTION_NODE             = 4,
			ENTITY_REFERENCE_NODE          = 5,
			ENTITY_NODE                    = 6,
			PROCESSING_INSTRUCTION_NODE    = 7,
			COMMENT_NODE                   = 8,
			DOCUMENT_NODE                  = 9,
			DOCUMENT_TYPE_NODE             = 10,
			DOCUMENT_FRAGMENT_NODE         = 11,
			NOTATION_NODE                  = 12
		};
		

	public:
		Node* insertBefore(Node* newChild, Node* refChild);
		Node* replaceChild(Node* newChild, Node* oldChild);
		Node* removeChild(Node* oldChild);
		Node* appendChild(Node* newChild);
		Node* cloneNode(bool deep);
		bool hasChildNodes();
		bool hasAttributes();
		void normalize();
		bool isSupported(const DOMString& feature, const DOMString& version);
		bool isSameNode(Node* other);
		DOMString lookupPrefix(const DOMString& namespaceURI);
		bool isDefaultNamespace(const DOMString& namespaceURI);
		DOMString lookupNamespaceURI(DOMString& prefix);
		bool isEqualNode(Node* arg);
		DOMObject getFeature(const DOMString& feature, const DOMString& version);
	
	protected:
		DOMString nodeName;
		DOMString nodeValue;
		
		enum NodeType nodeType;
		Node*         parentNode;
		NodeList      childNodes;
		Node*         firstChild;
		Node*         lastChild;
		Node*         previousSibling;
		Node*         nextSibling;
		NamedNodeMap  attributes;
		Document*     ownerDocument;
		DOMString     namespaceURI;
		DOMString     prefix;
		DOMString     localName;
};

class DocumentType : public Node {
	public:
		DOMString        name;
		NamedNodeMap     entities;
		NamedNodeMap     notations;
		DOMString        publicId;
		DOMString        systemId;
		DOMString        internalSubset;
};

class DOMImplementation {
	public:
		bool hasFeature(const DOMString& feature, const DOMString& version);
		DocumentType* createDocumentType(const DOMString& qualifiedName, const DOMString& publicId, const DOMString& systemId);
		Document* createDocument(const DOMString& namespaceURI, const DOMString& qualifiedName, const DocumentType& doctype);
		DOMObject* getFeature(const DOMString& feature, const DOMString& version);
};

// Introduced in DOM Level 3:
interface DOMConfiguration {
	public:
		void setParameter(const DOMString& name, const DOMUserData& value)
		DOMUserData getParameter(in DOMString name)
		bool canSetParameter(const DOMString& name, const DOMUserData& value);
	
	protected:
		DOMStringList  parameterNames;
};


class Attr : public Node {
	public:
		
		
	protected:
		DOMString  value;
		const DOMString  name;
		const bool    specified;
		const Element    ownerElement;
};

class Element : public Node {
	protected:	
		DOMString tagName;

	public:
		DOMString getAttribute(const DOMString& name);
		void      setAttribute(const getDOMString&); // FIXME: Throw exception?bute(const DOMString& name, const DOMString& value);
		void      removeAttribute(const DOMString& name);
		Attr      getAttributeNode(const DOMString& name);
		Attr      setAttributeNode(const Attr& newAttr);
		Attr      removeAttributeNode(const Attr& oldAttr);
		NodeList  getElementsByTagName(const DOMString& name);
		DOMString getAttributeNS(const DOMString& namespaceURI, const DOMString& localName);
		void      setAttributeNS(const DOMString& namespaceURI, const DOMString& qualifiedName, const DOMString& value);
		void      removeAttributeNS(const DOMString& namespaceURI, const DOMString& localName);
		Attr      getAttributeNodeNS(const DOMString& namespaceURI, const DOMString& localName);
		Attr      setAttributeNodeNS(const Attr& newAttr);
		NodeList  getElementsByTagNameNS(const DOMString& namespaceURI, const DOMString& localName);
		bool      hasAttribute(const DOMString& name);
		bool      hasAttributeNS(const DOMString& namespaceURI, const DOMString& localName);
};

class DocumentFragment : public Node { };


class Document : public Node {
	protected:
		DocumentType* doctype;
		DOMImplementation* implementation;
		Element documentElement;
		
	public:
		Element                createElement(const DOMString& tagName);
		DocumentFragment*      createDocumentFragment();
		Text*                  createTextNode(const DOMString& data);
		Comment*               createComment(const DOMString& data);
		CDATASection*          createCDATASection(const DOMString& data);
		ProcessingInstruction* createProcessingInstruction(const DOMString& target, const DOMString& data);
		Attr*                  createAttribute(const DOMString& name);
		EntityReference*       createEntityReference(const DOMString& name);
		NodeList*              getElementsByTagName(const DOMString& tagname);
		Node*                  importNode(Node* importedNode, bool deep);
		Element*               createElementNS(const DOMString& namespaceURI, const DOMString& qualifiedName);
		Attr*                  createAttributeNS(const DOMString& namespaceURI, const DOMString& qualifiedName);
		NodeList*              getElementsByTagNameNS(const DOMString& namespaceURI, const DOMString& localName);
		Element*               getElementById(const DOMString& elementId);
		Node*                  adoptNode(Node* source);
		void                   normalizeDocument();
		Node*                  renameNode(Node* n, const DOMString& namespaceURI, const DOMString& qualifiedName);
		
	protected:
		DOMString        inputEncoding;
		DOMString        xmlEncoding;
		bool             xmlStandalone;
		DOMString        xmlVersion;
		bool             strictErrorChecking;
		DOMString        documentURI;
		DOMConfiguration domConfig;
};

class CharacterData : public Node {
	protected:
	DOMString data;
	const unsigned long    length;
	DOMString substringData(unsigned long offset, unsigned long count);
	void appendData(const DOMString& arg);
	void insertData(unsigned long offset, const DOMString& arg);
	void deleteData(unsigned long offset, unsigned long count);
	void replaceData(unsigned long offset, unsigned long count, const DOMString& arg);
};


class Text : public CharacterData {
	Text splitText(unsigned long offset);
};

class CDATASection : public Text { };

class Notation : public Node {
	const DOMString publicId;
	const DOMString systemId;
};

class Entity : public Node {
	const DOMString* publicId;
	const DOMString* systemId;
	const DOMString* notationName;
};

class EntityReference : public Node { };

class ProcessingInstruction : public Node {
	DOMString target;
	DOMString data;
};

}

#endif // HAVE_QUICKDC_XML_PARSER
