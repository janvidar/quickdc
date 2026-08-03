/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */


#ifndef HAVE_QUICKDC_DCLOCKPICKER_H
#define HAVE_QUICKDC_DCLOCKPICKER_H

#include <string>
#include <string_view>

namespace DC {

class Lock {
	public:

		/**
		* This algorithm converts a lock into a key.
		* @return the key, or an empty string for a lock too short to derive
		*         one from. There is no lock of three characters or more whose
		*         key is empty, so the two cases do not overlap.
		*/
		static std::string calculateKey(std::string_view lock);

		/**
		* Create a lock.
		*/
		static std::string createLock();

		/**
		* Create pk. Well, we send the client version anyway.
		*/
		static std::string createPk();

};

}

#endif // HAVE_QUICKDC_DCLOCKPICKER_H
