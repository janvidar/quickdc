/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_ENTITY_H
#define HAVE_QUICKDC_HTTP_ENTITY_H


#include <string>
#include <vector>

#include "network/http/header.h"

namespace Samurai {
	namespace IO {
		class Buffer;
	}
}

namespace QuickDC {
class Transfer;
}

namespace Http {
class Header;
class HeaderList;

/**
 * The HTTP entity is either a HTTP request, or a HTTP response.
 * It contains a protocol part, header part, and optional body part.
 */
class Entity {
	public:

		enum Method { CONNECT, DELETE, GET, HEAD, OPTIONS, POST, PUT, TRACE, UNKNOWN };
		enum Protocol { HTTP_1_0, HTTP_1_1 };
		enum State { Waiting, Composing, Transfering, Done };

		Entity(bool request);
		virtual ~Entity();
	

	public:
		void addHeader(Http::Header* header);
		size_t countHeader(const char* name);
		Http::Header* getHeader(const char* name);

		void setMethod(enum Method method);
		void setURI(const char* uri);
		void setProtocol(enum Protocol protocol);

		void setMessage(const char*);
		void setCode(uint16_t);

		bool isRequest() const { return request; }
		
		/**
		 * Toggle whether CRLF is used for the headers, 
		 * or if only LF is used. ("\r\n" vs "\n").
		 */
		void setCRLF(bool toggle) { crlf = toggle; }
		bool isCRLF() const { return crlf; }

		/**
		 * Set the expected size of the attached body.
		 */
		void setBodySize(uint64_t sz);

		/**
		 * Returns the expected size of the attached body.
		 */
		uint64_t getBodySize() const { return bodySize; }
	
		/**
		 * Process the entity
		 */
		bool process();
		
	protected:
		void compose(Samurai::IO::Buffer* out);
		

	protected:
		HeaderList* headers;
		bool request;
		bool crlf;
		
		enum Protocol protocol;

		// Request contains these
		enum Method method;
		char* uri; /* raw encoded */

		// Response contains
		uint16_t code;
		char* msg;

		uint64_t bodySize;
		QuickDC::Transfer* transfer;
};

}

#endif // HAVE_QUICKDC_HTTP_ENTITY_H

