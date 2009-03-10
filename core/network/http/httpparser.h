/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_PARSER_H
#define HAVE_QUICKDC_HTTP_PARSER_H

#include "quickdc.h"
#include <vector>

namespace Samurai {
	namespace IO {
		class Buffer;
	}
}

namespace Http {

class Entity;
class Pipeline;

/**
 * This class will parse HTTP 0.9, 1.0 and 1.1 messages, and separate
 * header from body, handle transfer encodings etc.
 */
class Parser
{

	public:
		enum Mode { None, Detect, Header, DetectBody, Body };
		enum Status { StatusError, StatusMore, StatusDone };

		Parser(bool request);
		virtual ~Parser();

		/**
		 * Append more data into the parser.
		 * The parser will notify that it needs more data by return value,
		 * and automatically empty the IOBuffer.
		 */
		enum Status addEntity(Samurai::IO::Buffer* in, Http::Pipeline* pipeline);

		/**
		 * Return the entity currently used.
		 */
		Http::Entity* getEntity();

	protected:
		bool parseRequestLine(const char* line);
		bool parseStatusLine(const char* line);

		enum Status getLine();

	protected:
		enum Mode state;
		ssize_t length;
		Http::Entity* entity;
		Samurai::IO::Buffer* buffer;
		bool request;
		char* line;
};

}

#endif // HAVE_QUICKDC_HTTP_PARSER_H
