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
#include <signal.h>
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

/* Leaving the main loop is the only way the queue and caches get written, so a
   signal must not simply kill us. */
static volatile sig_atomic_t interrupted = 0;

static void onTerminate(int)
{
	interrupted = 1;
}

/**
 * Reports transfers as they start and finish.
 *
 * Transfers are not tied to a particular hub, so this is its own listener
 * rather than something bolted onto HubConnection.
 */
class TransferReporter : public QuickDC::TransferListener
{
	public:
		void EventTransferStarted(QuickDC::Transfer* xfer)
		{
			if (!xfer) return;

			console_printf("%s * %s started: %s (%s)\n",
				getTimestamp(),
				xfer->isDownload() ? "Download" : "Upload",
				name(xfer),
				getFormatSize(xfer->getLength()));
		}

		void EventTransferFinished(QuickDC::Transfer* xfer)
		{
			if (!xfer) return;

			char moved[32];
			char rate[32];
			formatSize(moved, sizeof(moved), xfer->getTransferred());
			formatSize(rate, sizeof(rate), xfer->getBps());

			console_printf("%s * %s finished: %s (%s at %s/s)\n",
				getTimestamp(),
				xfer->isDownload() ? "Download" : "Upload",
				name(xfer),
				moved, rate);
		}

		void EventTransferStopped(QuickDC::Transfer* xfer)
		{
			report(xfer, "stopped");
		}

		void EventTransferError(QuickDC::Transfer* xfer)
		{
			report(xfer, "failed");
		}

	private:
		/**
		 * getFormatSize() formats into a single static buffer, so two calls in
		 * one argument list would both render the last value. Take a copy.
		 */
		static void formatSize(char* out, size_t length, uint64_t bytes)
		{
			snprintf(out, length, "%s", getFormatSize(bytes));
		}

		/** The base name; a full path makes the line unreadable. */
		static const char* name(QuickDC::Transfer* xfer)
		{
			if (!xfer->getFile()) return "";

			const std::string& path = xfer->getFile()->getName();
			size_t cut = path.find_last_of("/\\");

			return (cut == std::string::npos) ? path.c_str() : path.c_str() + cut + 1;
		}

		/** Shared by the two ways a transfer can end short. */
		void report(QuickDC::Transfer* xfer, const char* what)
		{
			if (!xfer) return;

			char moved[32];
			char expected[32];
			formatSize(moved, sizeof(moved), xfer->getTransferred());
			formatSize(expected, sizeof(expected), xfer->getLength());

			console_printf("%s * %s %s: %s (%s of %s)\n",
				getTimestamp(),
				xfer->isDownload() ? "Download" : "Upload",
				what,
				name(xfer),
				moved, expected);
		}
};

static TransferReporter transferReporter;

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
	char* url_ = 0;

	/*
	 * An address that already names a protocol is used exactly as given. The
	 * query is part of it - an ADCS keyprint lives there - and rebuilding the
	 * address from a host and a port would throw that away.
	 */
	if (argc > 1 && strstr(argv[1], "://"))
	{
		url_ = strdup(argv[1]);
	}
	else if (argc > 1)
	{
		int port = 411;
		char* host = argv[1];

		char* sport = strrchr(host, ':');
		if (sport)
		{
			size_t n = 1;
			for (; sport[n] && isdigit(sport[n]); n++) {} /* ignore trailing data */
			sport[0] = '\0';
			sport[n] = '\0';
			port = Samurai::Util::Convert::to_int32(&sport[1]);
		}

		if (*host && port > 0 && port <= 65535)
		{
			size_t url_size = strlen(host) + 24; /* "dchub://" + ":" + port + \0 */
			url_ = (char*) malloc(url_size);
			snprintf(url_, url_size, "dchub://%s:%d", host, port); /* default = dc, not adc */
		}
	}

	Console::getInstance();

	if (!url_)
	{
		console_printf("Usage: %s {url|host[:port]}\n", argv[0]);
		exit(-1);
	}

	Samurai::IO::Net::URL* url = new Samurai::IO::Net::URL(url_);

	/* A URL with no port of its own still gets the one the shorthand assumes. */
	if (url->isValid() && !url->getPort())
	{
		std::string rebuilt = url->getScheme();
		rebuilt += "://";
		rebuilt += url->getHost().toString();
		rebuilt += ":411";
		rebuilt += url->getFile();

		delete url;
		free(url_);
		url_ = strdup(rebuilt.c_str());
		url = new Samurai::IO::Net::URL(url_);
	}

	if (!url->isValid())
	{
		console_printf("'%s' is not an address I can use.\n", url_);
		exit(-1);
	}

	core = new QuickDC::Core();
	core->transfers->addListener(&transferReporter);
	hub = new HubConnection(url);

	if (console_initialize() == -1) return -1;

	console_printf("Welcome to " PRODUCT " " VERSION " test command shell\n");
	console_printf("Type `/connect' to connect to `%s', or `/help' to see available commands.\n\n", url_);

	signal(SIGINT, onTerminate);
	signal(SIGTERM, onTerminate);
	signal(SIGHUP, onTerminate);

	running = true;
	char* line = 0;
	while (running && !interrupted) {
		core->run();

		/* One wakeup can carry more than one line, so drain the queue rather
		   than handling a single line per pass. */
		while (running && (line = Console::getInstance()->getLine()) != 0)
			processKeyboard(line);
	}

	if (interrupted)
		console_printf("\nInterrupted, shutting down.\n");

	Console::shutdown();
	delete hub;
	core->transfers->removeListener(&transferReporter);
	delete core;
	free(url_);
	delete url;
}
