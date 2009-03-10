/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/http/server.h"
#include "network/http/entity.h"
#include "network/http/request.h"
#include "network/http/response.h"

Http::Server::Server()
{
	
}

Http::Server::~Server()
{
	
}

void onDelete (Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;	
}

void onGet    (Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;	
}

void onOptions(Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;
}

void onPost   (Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;
}

void onPut    (Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;
}

void onTrace  (Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;
}

void onHead   (Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;
}

void onError  (Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;
}

void onConnect(Http::Request* req, Http::Response* res)
{
	(void) req;
	(void) res;
}


void Http::Server::service(Http::Request* req, Http::Response* res)
{
	switch (req->getMethod())
	{
		case Entity::DELETE:
			onDelete(req, res);
			break;
			
		case Entity::GET:
			onGet(req, res);
			break;
			
		case Entity::HEAD:
			onHead(req, res);
			break;
			
		case Entity::OPTIONS:
			onOptions(req, res);
			break;
			
		case Entity::POST:
			onPost(req, res);
			break;
			
		case Entity::PUT:
			onPut(req, res);
			break;
			
		case Entity::TRACE:
			onTrace(req, res);
			break;
			
		case Entity::CONNECT:
			onConnect(req, res);
			break;

			
		case Entity::UNKNOWN:
			onError(req, res);
			break;

	}
}
