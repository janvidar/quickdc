/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "api/core.h"
#include "test/cli/console.h"
#include "test/cli/util.h"
#include "test/cli/commands.h"
#include "test/cli/hubconnection.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <unistd.h>

#include <samurai/io/net/bandwidth.h>
#include <samurai/io/file.h>
#include <samurai/io/net/url.h>

#include "network/adc/server/adchub.h"
#include "network/adc/server/adchubuser.h"
#include "network/connection.h"
#include "network/connectionmanager.h"
#include "share/searchrequest.h"
#include "share/sharemanager.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "network/transfer.h"
#include "network/transfermanager.h"


#define MAXLINE 1024

QuickDC::Core* core;
HubConnection* hub;
bool running;

class CoreListener : public QuickDC::DCServerListener
{
	public:
		bool EventGotConnection(const char* address, uint16_t port)
		{
			console_printf(" * NET: Got connection from %s:%d\n", address, port);
			return true;
		}
};

static void processKeyboard(const char* line)
{
	if (line[0] == '/')
	{
		if (strlen(line) > 2 && line[1] == '/')
		{
			hub->sendChatMessage(&line[1]);
		}
		else
		{
			parseCommand(line, strlen(line));
		}
	}
	else
	{
		if (strlen(line))
		{
			hub->sendChatMessage(line);
		}
	}
}



int main(int argc, char** argv) {
	// test hubs:
	// - adc://dcdev.no-ip.org:16591
	// - dcdev://dev.myhub.org:666
	int port = 411;
	char* host = 0;
	const char* protocol = "dchub";
	if (argc > 1)
	{
		host = argv[1];
		if (strlen(host) > 8 && strncasecmp(host, "dchub://", 8) == 0)       host = &host[8]; /* default = dc, not adc */
		else if (strlen(host) > 6 && strncasecmp(host, "adc://", 6) == 0)  { host = &host[6]; protocol = "adc"; }
		else if (strlen(host) > 6 && strncasecmp(host, "adcs://", 7) == 0) { host = &host[7]; protocol = "adcs"; }

		char* sport = strrchr(argv[1], ':');
		if (sport)
		{
			size_t n = 1;
			for (; sport[n] && isdigit(sport[n]); n++) {} /* ignore trailing data */
			sport[0] = '\0';
			sport[n] = '\0';
			port = quickdc_atoi(&sport[1]);
		}
	}

	Console::getInstance();
	
	if (port <= 0 || port > 65535 || !host)
	{
		console_printf("Usage: %s host[:port]\n", argv[0]);
		exit(-1);
	}
	
	char* url_ = (char*) malloc(strlen(host)+strlen(protocol)+10); /* 10 = "://" + ":" + "12345" + \0 */
	sprintf(url_, "%s://%s:%d", protocol, host, port);
	Samurai::IO::Net::URL* url = new Samurai::IO::Net::URL(url_);
	core = new QuickDC::Core();
	hub = new HubConnection(url);

	if (console_initialize() == -1) return -1;

	console_printf("Welcome to " PRODUCT " " VERSION " test command shell\n");
	console_printf("Type `/connect' to connect to `%s:%d', or `/help' to see available commands.\n\n", host, port);
	
	running = true;
	char* line = 0;
	while (running) {
		core->run();
		line = Console::getInstance()->getLine();
		if (line) processKeyboard(line);
	}
	
	delete hub;
	delete core;
	free(url_);
	delete url;
}

