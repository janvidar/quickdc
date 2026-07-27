/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "console.h"

#include <string.h>
#include <strings.h>

#include <samurai/io/net/socket.h>
#include <samurai/io/net/socketevent.h>
#include <samurai/io/net/socketmonitor.h>
#include <samurai/io/net/inetaddress.h>
#include <samurai/io/net/socketaddress.h>

#ifdef SAMURAI_UNIX
#include <sys/poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <net/if.h>
#endif

#ifdef SAMURAI_WIN32
#include <conio.h>
#include <winsock2.h>
#endif

#include <unistd.h>
#include <assert.h>
#include <errno.h>

#define MAXLINE 1024


static char* last = 0;
static char* line = 0;
static size_t chars = 0;
#ifdef SAMURAI_UNIX
static struct pollfd pfd;
#endif



ConsoleOutput::ConsoleOutput()
{
}

ConsoleOutput::~ConsoleOutput()
{
}

ConsoleInput::ConsoleInput()
{
}

ConsoleInput::~ConsoleInput()
{
}

bool ConsoleInput::isOK()
{
	return false;
}


bool ConsoleInput::getByte(char& byte)
{
	byte = 0;
	return false;
}



#ifdef SAMURAI_UNIX
class ConsoleInputUnix : public ConsoleInput
{
	public:
		ConsoleInputUnix();
		~ConsoleInputUnix();
		
		bool isOK();
		bool getByte(char& byte);

		/** The terminal descriptor, for the reader thread to poll on. */
		int getFD() const { return fd; }

	protected:
		int fd;
};

class ConsoleOutputUnix : public ConsoleOutput
{
	public:
		ConsoleOutputUnix();
		virtual ~ConsoleOutputUnix();
};

ConsoleOutputUnix::ConsoleOutputUnix()
{
}

ConsoleOutputUnix::~ConsoleOutputUnix()
{
}

ConsoleInputUnix::ConsoleInputUnix() : fd(-1)
{
	fd = open("/dev/tty", O_RDONLY
// #ifdef O_DIRECT
// 		| O_DIRECT
// #endif
		);

	if (fd == -1)
	{
		fprintf(stderr, "Unable to open /dev/tty\n");
	}
	else if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
	{
		fprintf(stderr, "Unable to set /dev/tty non-blocking\n");
		close(fd);
		fd = -1;
	}
}

ConsoleInputUnix::~ConsoleInputUnix()
{
	if (fd != -1)
	{
		close(fd);
	}
}

bool ConsoleInputUnix::getByte(char& byte)
{
	char buf[1];
	ssize_t n = read(fd, buf, 1);
	if (n > 0)
	{
		byte = buf[0];
		return true;
	}
	byte = 0;
	return false;
}

bool ConsoleInputUnix::isOK()
{
	return fd != -1;
}
#endif


/**
 * Watches the read end of the wake socket pair. The bytes are meaningless -
 * receiving them is the whole point, because it is what breaks the monitor out
 * of its wait. They still have to be consumed: the monitor is level triggered,
 * so an undrained wake socket would report readable forever and spin the loop.
 */
class ConsoleWakeHandler : public Samurai::IO::Net::SocketEventHandler
{
	public:
		ConsoleWakeHandler(Console* console_) : console(console_) { }

	protected:
		void EventDataAvailable(const Samurai::IO::Net::Socket*)
		{
			console->drainWakeup();
		}

	private:
		Console* console;
};


Console* Console::instance = 0;
Console* Console::getInstance()
{
	if (!instance)
	{
		instance = new Console();
	}
	return instance;
}

void Console::shutdown()
{
	delete instance;
	instance = 0;
}

Console::Console()
	: input(0)
	, output(0)
	, last(0)
	, reader_running(false)
	, wake_read(-1)
	, wake_write(-1)
	, quit_read(-1)
	, quit_write(-1)
	, wakeSocket(0)
	, wakeHandler(0)
{
#ifdef SAMURAI_UNIX
	input  = new ConsoleInputUnix();
	output = new ConsoleOutputUnix();
	startReader();
#endif
}

Console::~Console()
{
	stopReader();

	if (wakeSocket)
	{
		Samurai::IO::Net::SocketMonitor::getInstance()->remove(wakeSocket);
		delete wakeSocket;              /* closes wake_read */
		wakeSocket = 0;
		wake_read = -1;
	}
	delete wakeHandler; wakeHandler = 0;

#ifdef SAMURAI_UNIX
	if (wake_read  != -1) close(wake_read);
	if (wake_write != -1) close(wake_write);
	if (quit_read  != -1) close(quit_read);
	if (quit_write != -1) close(quit_write);
#endif

	free(last);
	delete input;
	delete output;
}

#ifdef SAMURAI_UNIX

void Console::startReader()
{
	if (!input->isOK())
		return;

	int wake[2];
	int quit[2];

	/* A socket pair, not a pipe: the monitor can only watch sockets. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, wake) == -1)
	{
		fprintf(stderr, "Unable to create the console wakeup socket pair\n");
		return;
	}

	if (pipe(quit) == -1)
	{
		fprintf(stderr, "Unable to create the console shutdown pipe\n");
		close(wake[0]);
		close(wake[1]);
		return;
	}

	wake_read  = wake[0];
	wake_write = wake[1];
	quit_read  = quit[0];
	quit_write = quit[1];

	/* Non-blocking so drainWakeup() can read until it runs dry. */
	fcntl(wake_read, F_SETFL, O_NONBLOCK);

	Samurai::IO::Net::InetAddress addr("127.0.0.1");
	Samurai::IO::Net::InetSocketAddress sockaddr(addr, 0);

	/* This constructor adopts an existing descriptor and marks it Connected,
	   which is exactly the state the monitor expects to deliver reads for. */
	wakeSocket  = new Samurai::IO::Net::Socket(wake_read, sockaddr);
	wakeHandler = new ConsoleWakeHandler(this);
	wakeSocket->setEventHandler(wakeHandler);
	Samurai::IO::Net::SocketMonitor::getInstance()->add(wakeSocket);

	reader_running = true;
	reader = std::thread(&Console::readerLoop, this);
}

void Console::stopReader()
{
	if (!reader_running)
		return;

	reader_running = false;

	/* Wake the reader out of poll() rather than waiting for a keypress. */
	if (quit_write != -1)
	{
		char byte = 'q';
		ssize_t ignored = write(quit_write, &byte, 1);
		(void) ignored;
	}

	if (reader.joinable())
		reader.join();
}

/**
 * The reader thread. It only ever touches the terminal, the line queue and the
 * two descriptors it owns - never the socket monitor or anything else the main
 * thread is using.
 */
void Console::readerLoop()
{
	ConsoleInputUnix* tty = static_cast<ConsoleInputUnix*>(input);

	std::string current;

	const int ttyfd = tty->getFD();
	const int maxfd = (ttyfd > quit_read ? ttyfd : quit_read) + 1;

	for (;;)
	{
		/* select(), not poll(): on macOS poll() reports POLLNVAL for a
		   perfectly good terminal descriptor, so it can not be used to wait on
		   a tty. select() handles them correctly on every platform we build
		   for. */
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(ttyfd, &rfds);
		FD_SET(quit_read, &rfds);

		if (select(maxfd, &rfds, 0, 0, 0) == -1)
		{
			if (errno == EINTR) continue;
			return;
		}

		if (FD_ISSET(quit_read, &rfds))
			return;

		if (!FD_ISSET(ttyfd, &rfds))
			continue;

		char buf[MAXLINE];
		ssize_t n = read(ttyfd, buf, sizeof(buf));
		if (n <= 0)
		{
			if (n == -1 && (errno == EINTR || errno == EAGAIN))
				continue;
			return;
		}

		bool complete = false;
		for (ssize_t i = 0; i < n; i++)
		{
			char c = buf[i];
			if (c == '\n' || c == '\r')
			{
				std::lock_guard<std::mutex> guard(lock);
				lines.push_back(current);
				current.clear();
				complete = true;
			}
			else if (current.size() < MAXLINE - 1)
			{
				current += c;
			}
		}

		/* One byte is enough - the payload carries no meaning. */
		if (complete && wake_write != -1)
		{
			char byte = 'x';
			ssize_t ignored = write(wake_write, &byte, 1);
			(void) ignored;
		}
	}
}

void Console::drainWakeup()
{
	if (wake_read == -1)
		return;

	char buf[64];
	while (read(wake_read, buf, sizeof(buf)) > 0)
		/* keep going until it runs dry */;
}

#else // !SAMURAI_UNIX

/* No reader thread yet on this platform: the console layer here still polls
   with _kbhit(), so there is nothing to wake the main loop with. Porting this
   means giving the reader thread a ReadConsole() body and emulating
   socketpair() over a loopback TCP connection, since Windows has neither. */
void Console::startReader() { }
void Console::stopReader()  { }
void Console::readerLoop()  { }
void Console::drainWakeup() { }

#endif // SAMURAI_UNIX

char* Console::getLine()
{
	free(last);
	last = 0;

	std::lock_guard<std::mutex> guard(lock);
	if (lines.empty())
		return 0;

	last = strdup(lines.front().c_str());
	lines.pop_front();
	return last;
}

void console_puts(const char* line)
{
	if (chars > 0)
	{
		QDBG("chars is not null, forcing newline");
		printf("\n");
		printf("\r");
		for (size_t n = 0; n < chars; n++)
		{
			printf(" ");
		}
		printf("\r");
	}
	puts(line);
}


void console_printf(const char *format, ...)
{
	char buf[MAXLINE];
	memset(buf, 0, MAXLINE);
	va_list args;
	va_start(args, format);
	vsnprintf(buf, MAXLINE, format, args);
	va_end(args);

	if (buf[strlen(buf)-1] == '\n')
		buf[strlen(buf)-1] = 0;
	console_puts(buf);
}


int console_initialize()
{
#ifdef SAMURAI_UNIX
	pfd.fd = fileno(stdin);
	pfd.events = POLLIN | POLLERR | POLLHUP | POLLNVAL;
	if (fcntl(fileno(stdin), F_SETFL, O_NONBLOCK) == -1) {
		printf("Unable to set stdin non-blocking\n");
		return -1;
	}
#endif

	last = 0;
	line = new char[MAXLINE];
	memset(line, 0, MAXLINE);
	chars = 0;
	return 0;
}

char* console_get_line()
{

	if (last)
	{
		free(last);
		last = 0;
	}

#ifdef SAMURAI_UNIX
		char buffer[MAXLINE];
		memset(buffer, 0, MAXLINE);
		ssize_t size = read(fileno(stdin), buffer, MAXLINE);
		if (size == -1) return 0;

		/* Read and process chars */
		for (ssize_t x = 0; x < size; x++) {
			if (buffer[x] == '\n' || chars == MAXLINE-1) {
				line[chars++] = '\0';
				last = strdup(line);
				chars = 0;
				memset(line, 0, MAXLINE);
			} else {
				line[chars++] = buffer[x];
			}
		}
#endif // UNIX

#ifdef SAMURAI_WIN32
		if (_kbhit()) {
			int ch = _getche();
			switch (ch)
			{
				case 0x08:
					{
						if (chars > 0) {
							printf("  %c%c", 8, 8);
							line[chars--] = '\0';
							chars--;
						}
						break;
					}
				case 0x09:
					{	
						if (chars > 0) {
							printf("\nTab complete: '%s'\n", line);
							printf("%s", line);
						}
						break;
					}
				case '\n':
				case '\r':
					{
						line[chars++] = '\0';
						last = strdup(line);
						chars = 0;
						memset(line, 0, MAXLINE);
						if (ch != '\n') printf("\n");
						break;
					}
				default:
					line[chars++] = ch;
			}
		}
#endif

	return last;
}
