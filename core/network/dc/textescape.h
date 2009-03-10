/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCPROTOCOLESCAPER_H
#define HAVE_QUICKDC_DCPROTOCOLESCAPER_H

#include <string>

namespace DC {

class TextEscape
{
	public:

		/**
		 * Escape "dangerous" characters before sending them
		 * over the wire.
		 */
		static std::string escape(const std::string& str);
		
		/**
		 * Unescape "dangerous" characters after receiving them
		 * from the wire.
		 */
		static char* unescape(char* str);
		
#if 0
		/**
		* Modifies 'str_', by unescaping it, and returns a pointer 
		* to it.
		* @param max the initial string length of 'str_'
		* @return pointer to *str
		*/
		static char* unescape(char** str_, size_t max);
		
		/**
		* This will escape NMDC-specific characters so that they can be sent
		* safely over the protocol.
		*/
		static void escape(const char* old, size_t oldsize, char* str, size_t max);

		/**
		* Returns the length required for a buffer in order to escape 'str'.
		*/
		static size_t escapeLength(const char* str);
#endif
};

}

#endif // HAVE_QUICKDC_DCPROTOCOLESCAPER_H
