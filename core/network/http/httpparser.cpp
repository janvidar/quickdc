/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/http/httpparser.h"
#include "network/http/header.h"
#include "network/http/entity.h"
#include "network/http/pipeline.h"
#include <samurai/io/buffer.h>
#include <samurai/stdc.h>
#include <string>

#define MAX_HEADER_LINE_SIZE 16384

Http::Parser::Parser(bool request_)
{
	request = request_;
	state = None;
	length = -1;
	entity = 0;
	line = 0;
	buffer = new Samurai::IO::Buffer();
}

Http::Parser::~Parser()
{
	delete entity;
	delete buffer;
	delete[] line; line = 0;
}

enum Http::Parser::Status Http::Parser::addEntity(Samurai::IO::Buffer* input, Http::Pipeline* pipeline)
{
	QDBG("Http::Parser::addEntity()");

	buffer->append(input, input->size());
	input->remove(input->size());

	while (buffer->size())
	{
		
		switch (state)
		{
			case None:
			{
				entity = new Http::Entity(request);
				state = Detect;
				if (buffer->size() == 0)
					return StatusMore;
			}
			[[fallthrough]];
	
			case Detect:
			{
				/*
				 * The status has to be compared, not tested for truth: StatusError
				 * is 0 and StatusMore is 1, so "!getLine()" only caught the error
				 * and let a line that has not arrived in full fall through to be
				 * parsed as a null pointer.
				 */
				enum Http::Parser::Status got = getLine();
				if (got == StatusMore)  return StatusMore;
				if (got == StatusError) return StatusError;

				state = Header;
				bool ok;
				if (request)
				{
					ok = Http::Parser::parseRequestLine(line);
				}
				else
				{
					ok = Http::Parser::parseStatusLine(line);
				}
				
				if (!ok)
					return StatusError;
			}
			[[fallthrough]];
	
			case Header:
			{
				enum Http::Parser::Status ret = getLine();
				while (ret == StatusDone)
				{
					if (strlen(line))
					{
						entity->addHeader(new Http::Header(line));
					}
					else
					{
						state = DetectBody;
					}
					ret = getLine();
				}
	
				if (ret == StatusError)
					return StatusError;
	
				if (state != DetectBody)
					return StatusMore;
			}
			[[fallthrough]];
	
			case DetectBody:
			{
				QDBG("DetectBody");
				Http::Header* cl = entity->getHeader("Content-Length");
				Http::Header* te = entity->getHeader("Transfer-Encoding");
				if (te && te->getValue() && Samurai::Util::iequals(te->getValue(), "identity")) te = 0;
	
				if (cl || te)
				{
					/*
					 * Only a counted body can be consumed below, and a chunked
					 * one carries no count. Refusing it drops the connection,
					 * which is the honest outcome while the chunk framing is
					 * unimplemented - entering Body with a zero length would
					 * instead declare the entity complete and leave the chunk
					 * data to be read as whatever comes next on the stream.
					 */
					if (!cl || !cl->getValue())
					{
						QERR("Transfer-Encoding without a Content-Length is not supported");
						return StatusError;
					}
					entity->setBodySize(Samurai::Util::Convert::to_uint64(cl->getValue()));
					state = Body;
				}
				else
				{
					pipeline->push(entity);
					state = None;
					entity = 0;
					return StatusDone;
				}
				
				if (state != Body)
					return StatusMore;
			}
			[[fallthrough]];
	
			case Body:
			{
				QDBG("Body");

				/*
				 * The body is still not handed anywhere, but it has to be taken
				 * off the buffer: leaving it made the enclosing while(size())
				 * spin on a state that could never consume anything, which hangs
				 * the whole event loop rather than just this connection.
				 *
				 * A chunked body has no length to count down, and unpicking the
				 * chunk framing is not something to bolt on here, so that case
				 * ends the entity and lets the connection be dropped.
				 */
				const uint64_t want = entity->getBodySize();
				if (!want)
				{
					pipeline->push(entity);
					state = None;
					entity = 0;
					return StatusDone;
				}

				const size_t have = buffer->size();
				const size_t take = (have > want) ? static_cast<size_t>(want) : have;
				buffer->remove(take);
				entity->setBodySize(want - take);

				if (want - take)
					return StatusMore;

				pipeline->push(entity);
				state = None;
				entity = 0;
				return StatusDone;
			}
		}
	}

	/* Nothing left to look at, which is a read that came up empty rather than
	   anything being wrong with what it held. */
	return StatusMore;
}

Http::Entity* Http::Parser::getEntity()
{
	return entity;
}

enum Http::Parser::Status  Http::Parser::getLine()
{
	delete[] line; line = 0;

	size_t n = buffer->find('\n');
	if (n == Samurai::IO::Buffer::npos) {
		if (buffer->size() > MAX_HEADER_LINE_SIZE)
			return StatusError;
		return StatusMore;
	}

	if (state == Detect) {
		entity->setCRLF((n > 1 && buffer->at(n-1) == '\r'));
	}
	
	line = new char[n+1];
	buffer->pop(line, n);
	buffer->remove(n+1);


	if (n > 0 && line[n-1] == '\r') line[n-1] = '\0';
	else line[n] = '\0';

	return StatusDone;
}


/*
 * Method SP Request-URI SP HTTP-Version CRLF
 * Example:
 * GET /path/file HTTP/1.1
 */
bool Http::Parser::parseRequestLine(const char* line_) {
	const std::string line(line_ ? line_ : "");
	if (line.size() <= 8) return false;

	const size_t first = line.find(' ');
	if (first == std::string::npos) return false;

	const size_t second = line.find(' ', first + 1);
	if (second == std::string::npos || second + 1 >= line.size()) return false;

	const std::string method   = line.substr(0, first);
	const std::string uri      = line.substr(first + 1, second - first - 1);
	const std::string protocol = line.substr(second + 1);

	if (uri.empty()) return false;

	using Samurai::Util::iequals;

	if      (iequals(method, "GET"))     entity->setMethod(Http::Entity::GET);
	else if (iequals(method, "HEAD"))    entity->setMethod(Http::Entity::HEAD);
	else if (iequals(method, "POST"))    entity->setMethod(Http::Entity::POST);
	else if (iequals(method, "CONNECT")) entity->setMethod(Http::Entity::CONNECT);
	else if (iequals(method, "DELETE"))  entity->setMethod(Http::Entity::DELETE);
	else if (iequals(method, "OPTIONS")) entity->setMethod(Http::Entity::OPTIONS);
	else if (iequals(method, "TRACE"))   entity->setMethod(Http::Entity::TRACE);
	else if (iequals(method, "PUT"))     entity->setMethod(Http::Entity::PUT);
	else                                 entity->setMethod(Http::Entity::UNKNOWN);

	if (iequals(protocol, "HTTP/1.1")) entity->setProtocol(Http::Entity::HTTP_1_1);
	else                               entity->setProtocol(Http::Entity::HTTP_1_0);

	entity->setURI(uri.c_str());
	return true;
}

/*
 * HTTP-Version SP Status-Code SP Reason-Phrase CRLF.
 * Example:
 * HTTP/1.1 200 OK
 */
bool Http::Parser::parseStatusLine(const char* line_) {
	const std::string line(line_ ? line_ : "");
	if (line.size() <= 12) return false;

	const size_t first = line.find(' ');
	if (first == std::string::npos) return false;

	/* The separator before the reason phrase is required; the phrase itself is
	   not, and a proxy in front of an HTTP/2 server sends it empty. */
	const size_t second = line.find(' ', first + 1);
	if (second == std::string::npos) return false;

	const std::string version = line.substr(0, first);
	const std::string code    = line.substr(first + 1, second - first - 1);
	const std::string message = line.substr(second + 1);

	const uint16_t ncode = static_cast<uint16_t>(Samurai::Util::Convert::to_int32(code.c_str()));
	if (!ncode) return false;

	entity->setCode(ncode);
	entity->setMessage(message.c_str());

	if (Samurai::Util::iequals(version, "HTTP/1.1")) entity->setProtocol(Http::Entity::HTTP_1_1);
	else                                             entity->setProtocol(Http::Entity::HTTP_1_0);

	return true;
}
