/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/http/entity.h"
#include "network/http/header.h"
#include "network/http/responsecodes.h"
#include "network/transfer.h"
#include <stdlib.h>

Http::Entity::Entity(bool request_) : request(request_)
{
	bodySize = 0;
	headers = new Http::HeaderList();
	protocol = HTTP_1_1;
	method = UNKNOWN;
	uri = 0;
	code = 0;
	msg = 0;
	crlf = true;
	transfer = 0;
}

Http::Entity::~Entity()
{
	delete headers;
	free(uri);
	free(msg);
}

void Http::Entity::addHeader(Http::Header* header) {
	headers->add(header);
}

size_t Http::Entity::countHeader(const char* name) {
	return headers->count(name);
}


Http::Header* Http::Entity::getHeader(const char* name) {
	return headers->get(name);
}

void Http::Entity::setBodySize(uint64_t sz) {
	bodySize = sz;
}

void Http::Entity::setMethod(enum Method method_) { method = method_; }

void Http::Entity::setURI(const char* uri_) {
	if (uri) free(uri);
	uri = strdup(uri_);
}

void Http::Entity::setMessage(const char* msg_) {
	if (msg) free(msg);
	msg = strdup(msg_);
}

void Http::Entity::setCode(uint16_t code_) {
	code = code_;
}

void Http::Entity::setProtocol(enum Protocol protocol_) {
	protocol = protocol_;
}

bool Http::Entity::process()
{
	
	if (transfer)
	{
		transfer->exec();
		return false;
	}
	
	if (request)
	{
		
	}
	else
	{
	
	}
	return false;
}

void Http::Entity::compose(Samurai::IO::Buffer* out) {
	if (request) {
		if (method == UNKNOWN) return;
		switch (method) {
			case CONNECT:   out->append("CONNECT "); break;
			case DELETE:    out->append("DELETE ");  break;
			case GET:       out->append("GET ");     break;
			case HEAD:      out->append("HEAD ");    break;
			case OPTIONS:   out->append("OPTIONS "); break;
			case POST:      out->append("POST ");    break;
			case PUT:       out->append("PUT ");     break;
			case TRACE:     out->append("TRACE ");   break;
			default:
				break;
		}
		
		addHeader(new Header("Host", "localhost"));
		addHeader(new Header("Connection", "Keep-Alive"));
		addHeader(new Header("User-Agent", PRODUCT "/" VERSION));
		addHeader(new Header("Accept-Charset", "utf-8"));
		addHeader(new Header("Accept-Encoding", "deflate, gzip, x-gzip, identity, *;q=0"));

		out->append(uri);
		out->append((protocol == HTTP_1_1) ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n");
		
	} else {
		out->append((protocol == HTTP_1_1) ? "HTTP/1.1 " : "HTTP/1.0 ");
		out->append(quickdc_itoa(code, 10));
		out->append(' ');
		if (msg)
		{
			out->append(msg);
		}
		else
		{
			for (size_t n = 0; Http::responseCodes[n].code != 0; n++)
			{
				if (code == Http::responseCodes[n].code)
				{
					out->append(Http::responseCodes[n].message);
					break;
				}
			}
		}
		out->append("\r\n");
	}

	headers->compose(out);
	out->append("\r\n");

}
