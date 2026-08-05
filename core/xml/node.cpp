/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "xml/node.h"
#include <samurai/io/buffer.h>

XML::DOM::Node::Node() : parent(0) { }

/*
 * The parent is recorded whether there is one or not. Set only inside the branch,
 * it was left uninitialised for a node built without one - and ~Node() asks
 * whatever it holds to remove this child, so a root node tore itself down through
 * a wild pointer.
 */
XML::DOM::Node::Node(Node* parent_, const DOMString& name_)
	: name(name_)
	, parent(parent_)
{
	if (parent) parent->appendChild(this);
}

XML::DOM::Node::Node(Node* parent_, const DOMString& name_, const DOMString& value_)
	: name(name_)
	, value(value_)
	, parent(parent_)
{
	if (parent) parent->appendChild(this);
}

XML::DOM::Node::~Node() {
	while (hasChildNodes()) {
		Node* child = children.back();
		delete child;
	}
	
	if (parent) {
		parent->removeChild(this);
	}
}

/*
 * The removed child is what comes back, so it is named rather than read out of
 * the list again: erase() shifts everything after it down, so reading through the
 * iterator afterwards answers with the *next* child - and with none, when the one
 * removed was the last, which is a read off the end of the vector. The destructor
 * takes children off the back, so that was the ordinary case rather than the
 * awkward one.
 *
 * The child is detached as well as removed. Without that its own destructor asks
 * this node to remove it a second time, which finds nothing and throws - from a
 * destructor, where there is nowhere for the exception to go.
 */
XML::DOM::Node* XML::DOM::Node::removeChild(XML::DOM::Node* oldChild) {
	for (XML::DOM::NodeListIterator it = children.begin(); it != children.end(); it++) {
		if (*it == oldChild) {
			children.erase(it);
			if (oldChild) oldChild->parent = 0;
			return oldChild;
		}
	}
	throw XML::DOM::Exception(Exception::NOT_FOUND_ERR);
}

XML::DOM::Node* XML::DOM::Node::appendChild(XML::DOM::Node* newChild) {
	children.push_back(newChild);
	return newChild;
}

bool XML::DOM::Node::hasChildNodes() {
	return !children.empty();
}

bool XML::DOM::Node::hasAttributes() {
	return !attributes.empty();
}

const XML::DOM::DOMString& XML::DOM::Node::getAttribute(const XML::DOM::DOMString& name) {
	if (attributes.find(name) != attributes.end()) {
		return attributes[name];
	}
	throw XML::DOM::Exception(Exception::NOT_FOUND_ERR);
}

void XML::DOM::Node::setAttribute(const DOMString& name, const DOMString& value) {
	attributes[name] = value;
}

void XML::DOM::Node::removeAttribute(const DOMString& name) {
	if (attributes.find(name) != attributes.end()) {
		attributes.erase(name);
		return;
	}
	throw Exception(Exception::NOT_FOUND_ERR);
}

bool XML::DOM::Node::hasAttribute(const DOMString& name) {
	return (attributes.find(name) != attributes.end());
}


extern std::string quickdc_xml_escape(const std::string& str);

/* TODO: Escape XML */
void XML::DOM::Node::write(Samurai::IO::Buffer* out) {
	out->append('<');
	out->append(name);

	if (hasAttributes()) {
		NamedNodeMapIterator it = attributes.begin();
		for (; it != attributes.end(); it++) {
			out->append(' ');
			// out->append(quickdc_xml_escape(((*it).first()).c_str()));
			out->append("=\"");
			// out->append(quickdc_xml_escape((*it).second()));
			out->append('\"');
		}
	}

	if (hasChildNodes()) {
		out->append('>');

		NodeListIterator it = children.begin();
		for (; it != children.end(); it++) {
			(*it)->write(out);
		}

		out->append("</");
		out->append(name);
		out->append('>');
	} else {
		out->append(" />");
	}
}

XML::DOM::Document::Document() {
	this->doctype = "";
}


XML::DOM::Document::Document(const DOMString& doctype) {
	this->doctype = doctype;
}

XML::DOM::Document::~Document() {

}

bool XML::DOM::Document::haveDocType() const {
	return (doctype != "");
}


