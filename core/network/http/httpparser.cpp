/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/http/httpparser.h"
#include "network/http/header.h"
#include "network/http/entity.h"
#include "network/http/pipeline.h"
#include <samurai/io/buffer.h>

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
	
			case Detect:
			{
				if (!getLine())
					return StatusMore;
	
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
	
			case DetectBody:
			{
				QDBG("DetectBody");
				Http::Header* cl = entity->getHeader("Content-Length");
				Http::Header* te = entity->getHeader("Transfer-Encoding");
				if (te && !strcasecmp(te->getName(), "identity")) te = 0;
	
				if (cl || te)
				{
					if (cl && cl->getValue())
						entity->setBodySize(quickdc_atoull(cl->getValue()));
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
	
			case Body:
			{
				QDBG("Body");
				// FIXME: Calculate the size of the expected body size.
				// and setup a transfer job for the actual body transfer.
			}
		}
	}
	return StatusError;
}

Http::Entity* Http::Parser::getEntity()
{
	return entity;
}

enum Http::Parser::Status  Http::Parser::getLine()
{
	delete[] line; line = 0;

	int n = buffer->find('\n');
	if (n == -1) {
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
	if (strlen(line_) > 8) {
		char* line = strdup(line_);
		char* split = strchr(line, ' '); 

		if (!split || strlen(split) < 2) {
			free(line);
			return false;
		}

		split[0] = '\0';
		char* file  = &split[1];
		
		split = strchr(file, ' '); 
		if (!split || strlen(split) < 2) {
			free(line);
			return false;
		}
		
		split[0] = '\0';
		char* protocol = &split[1];
		
		if      (!strcasecmp(line, "GET"))     entity->setMethod(Http::Entity::GET);
		else if (!strcasecmp(line, "HEAD"))    entity->setMethod(Http::Entity::HEAD);
		else if (!strcasecmp(line, "POST"))    entity->setMethod(Http::Entity::POST);
		else if (!strcasecmp(line, "CONNECT")) entity->setMethod(Http::Entity::CONNECT);
		else if (!strcasecmp(line, "DELETE"))  entity->setMethod(Http::Entity::DELETE);
		else if (!strcasecmp(line, "OPTIONS")) entity->setMethod(Http::Entity::OPTIONS);
		else if (!strcasecmp(line, "TRACE"))   entity->setMethod(Http::Entity::TRACE);
		else if (!strcasecmp(line, "PUT"))     entity->setMethod(Http::Entity::PUT);
		else                                   entity->setMethod(Http::Entity::UNKNOWN);

		if (!strcasecmp(protocol, "HTTP/1.1")) entity->setProtocol(Http::Entity::HTTP_1_1);
		else                                   entity->setProtocol(Http::Entity::HTTP_1_0);
		
		entity->setURI(file);
		return true;
	}
	return false;
}

/*
 * HTTP-Version SP Status-Code SP Reason-Phrase CRLF.
 * Example:
 * HTTP/1.1 200 OK
 */
bool Http::Parser::parseStatusLine(const char* line_) {
	if (strlen(line_) > 12) {
		char* line = strdup(line_);
		char* split = strchr(line, ' '); 

		if (!split || strlen(split) < 2) {
			free(line);
			return false;
		}

		split[0] = '\0';
		char* code  = &split[1];
		
		split = strchr(code, ' '); 
		if (!split || strlen(split) < 2) {
			free(line);
			return false;
		}
		
		split[0] = '\0';
		char* message = &split[1];
		
		uint16_t ncode = (uint16_t) quickdc_atoi(code);
		if (ncode == 0) return false;

		entity->setCode(ncode);
		entity->setMessage(message);

		if (!strcasecmp(line, "HTTP/1.1")) entity->setProtocol(Http::Entity::HTTP_1_1);
		else                               entity->setProtocol(Http::Entity::HTTP_1_0);

		return true;
	}
	return false;
}




