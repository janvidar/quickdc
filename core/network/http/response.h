/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_RESPONSE_H
#define HAVE_QUICKDC_HTTP_RESPONSE_H

#include "quickdc.h"
#include "network/http/entity.h"

namespace QuickDC {
class IOBuffer;
}

namespace Http {
class Header;

class Response : public Entity {
	public:
		Response();
		Response(uint16_t code, const char* message);
		virtual ~Response();

		/**
		 * Write the request to an IOBuffer.
		 */
		void compose(QuickDC::IOBuffer* out);

	protected:
		uint16_t code;
		char* message;
		char* uri; /* raw encoded */
		uint8_t* entity_body;
		size_t entity_body_length;
};

}

#endif // HAVE_QUICKDC_HTTP_RESPONSE_H

