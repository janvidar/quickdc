/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */


#ifndef HAVE_QUICKDC_DCLOCKPICKER_H
#define HAVE_QUICKDC_DCLOCKPICKER_H

#include <stdio.h>
#include <string>

namespace DC {

class Lock {
	public:

		/**
		* This algorithm converts a lock into a key.
		* NOTE: must free() this string.
		*/
		static char* calculateKey(const char* lock, size_t length);
		
		/**
		* Create a lock.
		* NOTE: must free() this string.
		*/
		static char* createLock();
		
		/**
		* Create pk. Well, we send the client version anyway.
		* NOTE: must free() this string.
		*/
		static char* createPk();
		
};

}

#endif // HAVE_QUICKDC_DCLOCKPICKER_H
