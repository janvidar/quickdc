/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "debug/dbg.h"
#include "network/commandproxy.h"
#include "network/http/httpconnection.h"
#include "network/http/httpparser.h"
#include "network/http/pipeline.h"
#include <samurai/io/net/socket.h>
#include <samurai/io/buffer.h>

Http::Connection::Connection(Samurai::IO::Net::Socket* sock, bool client_) : QuickDC::Connection(sock, "HTTP"),  QuickDC::CommandProxy(sock), client(client_)
{
	QDBG("Http::Connection: - constructed.");
	
	/* Set up a parser for parsing requests */
	parser_in = new Http::Parser(!client);
	parser_out = new Http::Parser(client);
	pipeline_in  = new Http::Pipeline();
	pipeline_out = new Http::Pipeline();
}


Http::Connection::~Connection()
{
	QDBG("Http::Connection: - destructing.");
	close();
	delete parser_in;
	delete parser_out;
	delete pipeline_in;
	delete pipeline_out;
}


void Http::Connection::close()
{
	QDBG("Http::Connection::close(): socket=%p, mgr=%p", QuickDC::Connection::socket, manager);
	QuickDC::Connection::socket->setEventHandler(0);
	delete QuickDC::Connection::socket; QuickDC::Connection::socket = 0;
	if (manager) manager->remove(this);
}


void Http::Connection::EventDataAvailable(const Samurai::IO::Net::Socket*)
{
	QDBG(__PRETTY_FUNCTION__);
	read();
}


bool Http::Connection::write()
{
	QDBG(__PRETTY_FUNCTION__);
	return false;
}


bool Http::Connection::read()
{
	QDBG(__PRETTY_FUNCTION__);
	size_t bufsize = QuickDC::Connection::socket->getReceiveBufferSize();
	char* buffer = new char[bufsize];
	int n = QuickDC::Connection::socket->read(buffer, bufsize);

	if (n == -1)
	{
		delete[] buffer;
		return false;
	}

	readbuffer->append(buffer, n);
	delete[] buffer;

	enum Http::Parser::Status status = parser_in->addEntity(readbuffer, pipeline_in);

	if (status == Http::Parser::StatusError)
	{
		close();
		return false;
	}

	processPipeline();

	return true;
}


void Http::Connection::processPipeline()
{
	QDBG(__PRETTY_FUNCTION__);
	
	if (isServer())
	{
		QDBG("Pipeline process. size of incoming queue=%d", pipeline_in->size());
		if (pipeline_in->size())
		{
			
		}
	}
}


void Http::Connection::EventCanWrite(const Samurai::IO::Net::Socket*)
{
	QDBG(__PRETTY_FUNCTION__);
	processPipeline();
	write();
}



void Http::Connection::EventDisconnected(const Samurai::IO::Net::Socket*)
{
	QDBG(__PRETTY_FUNCTION__);
	close();
}


void Http::Connection::EventError(const Samurai::IO::Net::Socket*, Samurai::IO::Net::SocketError, const char*)
{
	QDBG(__PRETTY_FUNCTION__);
	close();
}


void Http::Connection::EventHostLookup(const Samurai::IO::Net::Socket*) { }
void Http::Connection::EventHostFound(const Samurai::IO::Net::Socket*) { }
void Http::Connection::EventConnecting(const Samurai::IO::Net::Socket*) { }

void Http::Connection::EventConnected(const Samurai::IO::Net::Socket*)
{
	manager->add(this);
}

void Http::Connection::EventTimeout(const Samurai::IO::Net::Socket*) { }


