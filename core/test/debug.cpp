/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdio.h>

#include <samurai/samurai.h>

#define UNIX

#ifdef UNIX
#include <sys/poll.h>
#endif
#ifdef WIN32
#include <conio.h>
#endif

#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "network/dc/hubsession.h"
#include "config/preferences.h"

#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/buffer.h>

#include "network/dc/dccommandparser.h"
#include "network/dc/dccommandproxy.h"



using namespace Samurai::IO::Net;

static bool running = true;

class Telnet : public SocketEventHandler {
	protected:
			Socket* socket;
			
	public:
		Telnet(const char* host, int port) {
			socket = new Socket(this, host, port);
			
		}
		
		virtual ~Telnet() {
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
		
		void EventHostLookup(const Socket*) {
			status("Looking up host...");
		}
		
		void EventHostFound(const Socket*) {
			status("Host found.");
		}
		
		void EventConnecting(const Socket*) {
			status("Connecting ...");
		}
		
		void EventConnected(const Socket*) {
			status("Connected.");
		}
		
		void EventTimeout(const Socket*) {
			status("Connection timed out...");
		}
		
		void EventDisconnected(const Socket*) {
			status("Disconnected...");
			disconnect();
			exit((int) 0);
		}
		
		void EventDataAvailable(const Socket*) {
			char* buffer = new char[1024];
			size_t bytes = socket->read(buffer, 1024);
			buffer[bytes] = 0;

			for (size_t x = 0; x < bytes ; x++) 
				if (buffer[x] == '|') buffer[x] = '\n';

			printf("%s", buffer);
			delete[] buffer;
		}

		void EventCanWrite(const Socket*) {
			/* can write */
		}
		
		void EventError(const Socket*, enum SocketError error, const char* msg) { 
			printf(" ERROR: %s\n", msg);
			disconnect();
			exit((int) error);
		}
	
};

int main(int argc, char** argv) {
	int port = 0;
	char* host = 0;
	if (argc > 2) port = atoi(argv[2]);
	if (argc > 1) host = argv[1];
	if (argc < 2 || port <= 0 || port > 65535) {
		printf("Usage: %s host port\n", argv[0]);
		printf("A simple DC telnet debugger\n");
		exit(-1);
	}

	Telnet telnet(host, port);
	telnet.connect();

#ifdef UNIX
	struct pollfd pfd;
	pfd.fd = fileno(stdin);
	pfd.events = POLLIN | POLLERR | POLLHUP | POLLNVAL;

	if (fcntl(fileno(stdin), F_SETFL, O_NONBLOCK) == -1) {
		printf("Unable to set stdin non-blocking\n");
		return -1;
 	}
#endif // UNIX


#ifdef WIN32
#define MAXLINE 1024
	char* line = new char[MAXLINE];
	int chars = 0;
#endif

	while (running) {
		SocketMonitor::getInstance()->wait(50);
	
#ifdef UNIX
		int pollret = poll(&pfd, 1, 50);
		if (pollret == 0) continue;
		else if (pollret == -1) break;
		else {
			char* buffer = new char[1024];
			fgets(buffer, 1024, stdin);

			size_t size = strlen(buffer);
			for (size_t x = 0; x < size; x++) 
				if (buffer[x] == '\n') buffer[x] = '|';

			telnet.write(buffer, size);
			delete[] buffer;
		}
#endif // UNIX

#ifdef WIN32
		if (_kbhit()) {
			int ch = _getche();
			if (ch == '\n' || ch == '\r' || chars == MAXLINE-2) {
				line[chars++] = '|';
				line[chars++] = '\0';
				
				telnet.write(line, chars);
				chars = 0;
				line[0] = '\0';
			} else {
				line[chars++] = ch;
			}
		}
#endif
	}
	
}
