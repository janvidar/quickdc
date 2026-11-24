/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_CONSOLE_H
#define HAVE_QUICKDC_CLI_CONSOLE_H

#include "quickdc.h"
#include "debug/dbg.h"

#include <memory>
#include <string>
#include <deque>
#include <mutex>
#include <thread>

namespace Samurai {
	class EventLoop;
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
}

class ConsoleWakeHandler;

class ConsoleOutput
{
	public:
		ConsoleOutput();
		virtual ~ConsoleOutput();
		
		void writeLine(const char* text);
};

class ConsoleInput
{
	public:
		ConsoleInput();
		virtual ~ConsoleInput();

		virtual bool isOK();
		
		virtual bool getByte(char& byte);
};

/**
 * Console input is read by a dedicated thread, because the terminal cannot be
 * waited on together with the sockets: SocketMonitor only understands sockets
 * (and on Windows select() genuinely refuses anything else), so there is no
 * portable way to hand it a tty.
 *
 * Instead the reader thread blocks on the terminal, assembles whole lines, and
 * pokes a loopback socket pair. That pair *is* a socket, so the existing
 * monitor can watch it unmodified, and the main loop wakes the moment a line is
 * ready instead of noticing it on the next poll tick.
 *
 *     reader thread                     main thread
 *       read(tty) ------------------->  monitor->wait()
 *       queue the line                    wakes on the wake socket
 *       write(wake socket)                getLine() drains the queue
 */
class Console
{
	public:
		static Console* getInstance();
		static void shutdown();

		/**
		 * Returns true if we can read from the console either via stdin or
		 * /dev/tty.
		 */
		bool isReadable();
		
		/**
		 * Returns true if we can write text to the console (like, printf).
		 */
		bool isWritable();
		
		/**
		 * Returns true if we can write text to a given location on the
		 * console output buffer.
		 */
		bool isRelocateable();
		
		/**
		 * Clear the screen.
		 */
		void clear();
		
		/**
		 * Return the next complete line the reader thread has queued, or null
		 * if there is none. Call it until it returns null - a single wakeup can
		 * carry several lines.
		 *
		 * The returned string stays valid until the next call.
		 */
		char* getLine();

		/**
		 * Drain the wake socket. Called when the monitor reports it readable;
		 * the byte itself carries no information, it only breaks the wait.
		 */
		void drainWakeup();

		/**
		 * Have a line that is ready wake 'loop' out of its wait. Lines are
		 * queued whether or not this is called; without it they are only
		 * noticed on the next poll tick.
		 */
		void watch(Samurai::EventLoop& loop);

	private:
		Console();
		~Console();
		static Console* instance;

		void readerLoop();
		void startReader();
		void stopReader();

		ConsoleInput*  input;
		ConsoleOutput* output;

		/* Filled by the reader thread, drained by the main thread. */
		std::mutex lock;
		std::deque<std::string> lines;
		char* last;

		std::thread reader;
		bool reader_running;

		/* Reader -> main. A socket pair rather than a pipe so that the socket
		   monitor can watch it like any other socket. */
		int wake_read;
		int wake_write;

		/* Main -> reader, so shutdown does not have to wait for a keypress. */
		int quit_read;
		int quit_write;

		std::shared_ptr<Samurai::IO::Net::Socket> wakeSocket;
		ConsoleWakeHandler* wakeHandler;
};

extern int console_initialize(); 
extern char* console_get_line();

extern void console_puts(const char* line);
extern void console_printf(const char* format, ...) QUICKDC_PRINTF_LIKE(1, 2);

#endif // HAVE_QUICKDC_CLI_CONSOLE_H

