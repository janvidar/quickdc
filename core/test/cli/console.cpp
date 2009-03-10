/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "console.h"

#include <string.h>
#include <strings.h>

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


Console* Console::instance = 0;
Console* Console::getInstance()
{
	if (!instance)
	{
		instance = new Console();
	}
	return instance;
}

Console::Console() : input(0), output(0)
{
#ifdef SAMURAI_UNIX
	input  = new ConsoleInputUnix();
	output = new ConsoleOutputUnix();
#endif
}

Console::~Console()
{
	delete input;
	delete output;
}

char* Console::getLine()
{
	static size_t chars = 0;
	static char line[MAXLINE] = {0, };
	static char* last = 0;
	
	if (last)
	{
		free(last);
		last = 0;
	}
	
	char c;
	bool ok = input->getByte(c);
	
	if (!ok)
		return 0;
	
	switch (c)
	{
		case '\t':
			puts("<tab>");
			break;
	}
	
	line[chars] = c;
	
	if (c == '\n' || chars == MAXLINE-1)
	{
		line[chars++] = '\0';
		last = strdup(line);
		
		memset(line, 0, chars);
		chars = 0;
	}
	else
		chars++;
	
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
