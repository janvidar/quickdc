/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_REQUEST_H
#define HAVE_QUICKDC_HTTP_REQUEST_H

#include "quickdc.h"
#include "network/http/entity.h"

namespace QuickDC {
class IOBuffer;
}

namespace Http {

class Header;

class Request : public Http::Entity
{
	public:
		Request();
		Request(enum Method, const char* uri, enum Protocol);
		virtual ~Request() { }

		enum Method getMethod() const;
		void setMethod(enum Method method);
		void setURI(const char* uri);
		void setProtocol(enum Protocol protocol);

		/**
		 * Write the request to an IOBuffer.
		 */
		void compose(QuickDC::IOBuffer* out);

	protected:

		enum Method method;
		enum Protocol protocol;
		
		char* uri; /* raw encoded */
		

		uint8_t* entity_body;
		size_t entity_body_length;
		bool crlf;

	friend class Parser;
};

}

#endif // HAVE_QUICKDC_HTTP_REQUEST_H
