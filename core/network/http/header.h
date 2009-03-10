/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_HEADER_H
#define HAVE_QUICKDC_HTTP_HEADER_H

#include "quickdc.h"
#include <vector>
#include <samurai/io/buffer.h>

namespace Http {

class Header {
	public:
		Header(const char* line);
		Header(const char* name, const char* value);
		virtual ~Header();

		char* getName() const { return name; }
		char* getValue() const { return value; }
		
		bool isValid() const;

	protected:
		char* name;
		char* value;
};

class HeaderList {
	public:
		HeaderList();
		virtual ~HeaderList();

		/**
		 * Add a header to the request/response.
		 */
		void add(Http::Header* header);

		/**
		 * get a header by it's name.
		 * FIXME: If multiple header entries with the same name
		 * exists, this method will return the first entry in the
		 * list. This behaviour might therefore be undefined.
		 * It is therefore advicable to only use this method,
		 * when count() returns 1.
		 */
		Http::Header* get(const char* name);
		
		/**
		 * Returns the number of headers with the given name
		 */
		size_t count(const char* name);

		void compose(Samurai::IO::Buffer* out);
	
	protected:
		std::vector<Http::Header*> headers;
};

}

#endif // HAVE_QUICKDC_HTTP_HEADER_H
