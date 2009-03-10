/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_SERVER_H
#define HAVE_QUICKDC_HTTP_SERVER_H

namespace Http {

class Request;
class Response;

class Server
{
	public:
		Server();
		virtual ~Server();
	
	protected:
		virtual void onDelete (Http::Request* req, Http::Response* res);
		virtual void onGet    (Http::Request* req, Http::Response* res);
		virtual void onOptions(Http::Request* req, Http::Response* res);
		virtual void onPost   (Http::Request* req, Http::Response* res);
		virtual void onPut    (Http::Request* req, Http::Response* res);
		virtual void onTrace  (Http::Request* req, Http::Response* res);
		virtual void onHead   (Http::Request* req, Http::Response* res);
		virtual void onError  (Http::Request* req, Http::Response* res);
		virtual void onConnect(Http::Request* req, Http::Response* res);
	
	protected:
		void service(Http::Request* req, Http::Response* res);
};

}

#endif // HAVE_QUICKDC_HTTP_SERVER_H
