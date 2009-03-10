/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_CONSOLE_H
#define HAVE_QUICKDC_CLI_CONSOLE_H

#include "quickdc.h"

/**
 * FIXME: The API is probably too simple.
 */
class TextInputEventHandler
{
	public:
		virtual ~TextInputEventHandler() { }

		/**
		 * This requests a tab completion to be performed
		 * on the given string at the given position (pos).
		 * The string can be modified, and the return value will
		 * alert if the string cannot be modified (no match), or
		 * if tab completion was successful.
		 */
		virtual bool EventTabComplete(char* str, size_t& pos) = 0;
};

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
		
	private:
		TextInputEventHandler* handler;
};

class Console
{
	public:
		static Console* getInstance();

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
		
		char* getLine();
		
	private:
		Console();
		~Console();
		static Console* instance;
		
		ConsoleInput*  input;
		ConsoleOutput* output;
};

extern int console_initialize(); 
extern char* console_get_line();

extern void console_puts(const char* line);
extern void console_printf(const char* format, ...);

#endif // HAVE_QUICKDC_CLI_CONSOLE_H

