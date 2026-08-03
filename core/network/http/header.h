/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_HEADER_H
#define HAVE_QUICKDC_HTTP_HEADER_H

#include "quickdc.h"
#include <optional>
#include <string>
#include <vector>
#include <samurai/io/buffer.h>

namespace Http {

class Header {
	public:
		Header(const char* line);
		Header(const char* name, const char* value);
		virtual ~Header();

		/**
		 * @return the name and the value, or null for either when the line was
		 *         not a header at all.
		 */
		const char* getName() const { return name.empty() ? 0 : name.c_str(); }
		const char* getValue() const { return value ? value->c_str() : 0; }

		bool isValid() const;

	protected:
		/*
		 * A field value is allowed to be empty, so having no value and having an
		 * empty one are different things and the value is optional rather than
		 * just possibly empty.
		 */
		std::string name;
		std::optional<std::string> value;
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
