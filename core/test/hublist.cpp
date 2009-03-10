/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "api/core.h"
#include "network/dc/hubsession.h"
#include "config/preferences.h"

#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/buffer.h>

#include "network/dc/dccommandparser.h"
#include "network/dc/dccommandproxy.h"

static bool running = true;

class Connection : public Samurai::IO::Net::SocketEventHandler {
	protected:
		Samurai::IO::Net::Socket* socket;
			
	public:
		Connection(const char* host, int port) {
			socket = new Samurai::IO::Net::Socket(this, host, port);
		}
		
		virtual ~Connection() {
			delete socket;
		}
		
		void connect() {
			socket->connect();	
		}
		
		void disconnect() {
			delete socket;
			socket = 0;
		}

		void write(char* buffer, size_t size) {
			socket->write(buffer, size);
		}


	protected:
		void status(const char* msg) {
			printf("*** %s\n", msg);
		}
		
		void EventHostLookup(const Samurai::IO::Net::Socket*) {
			status("Looking up host...");
		}
		
		void EventHostFound(const Samurai::IO::Net::Socket*) {
			status("Host found.");
		}
		
		void EventConnecting(const Samurai::IO::Net::Socket*) {
			status("Connecting ...");
		}
		
		void EventConnected(const Samurai::IO::Net::Socket*) {
			status("Connected.");
			if (socket->TLSInitialize(false)) {
				socket->TLSsendHandshake();
			} else {
				status("Unable to initialize SSL socket");
				running = false;
			}
		}
		
		void EventTLSConnected(const Samurai::IO::Net::Socket*) {
			status("TLS Connected -- Secure connection established.");
			socket->write("HEAD / HTTP/1.0\r\n\r\n", 20);
		}
	
		void EventTLSDisconnected(const Samurai::IO::Net::Socket*) {
			status("TLS disconnect -- No longer secure connection.");
		}

		
		void EventTimeout(const Samurai::IO::Net::Socket*) {
			status("Connection timed out...");
			running = false;
		}
		
		void EventDisconnected(const Samurai::IO::Net::Socket*) {
			status("Disconnected...");
			running = false;
		}
		
		void EventDataAvailable(const Samurai::IO::Net::Socket*) {
			char* buffer = new char[1024];
			size_t bytes = socket->read(buffer, 1024);
			buffer[bytes] = 0;

			/*
			for (size_t x = 0; x < bytes ; x++) 
				if (buffer[x] == '|') buffer[x] = '\n';
			*/
			printf("%s\n", buffer);
			delete[] buffer;
		}

		void EventCanWrite(const Samurai::IO::Net::Socket*) {
			/* can write */
		}
		
		void EventError(const Samurai::IO::Net::Socket*, enum Samurai::IO::Net::SocketError /*error*/, const char* msg) {
			printf(" ERROR: %s\n", msg);
			running = false;
		}
	
};

int main(int argc, char** argv) {
	int port = 0;
	char* host = 0;
	if (argc > 2) port = atoi(argv[2]);
	if (argc > 1) host = argv[1];
	if (argc < 2 || port <= 0 || port > 65535) {
		printf("Usage: %s url\n", argv[0]);
		printf("A simple DC hublist parser\n");
		exit(-1);
	}
	
	QuickDC::Core* core = new QuickDC::Core();

	Connection con(host, port);
	con.connect();
	while (running) {
		Samurai::IO::Net::SocketMonitor::getInstance()->wait(10);
		
	}
	
	delete core;
}

