/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCTAG_H
#define HAVE_QUICKDC_DCTAG_H

#include "quickdc.h"

#include <string>
#include <string_view>

namespace DC {

class Tag
{
	public:
		Tag(const char* string);
		virtual ~Tag();

	public:
		bool isOK();

		/**
		 * @return the client and version, or null when the tag named no client.
		 *         Owned by the tag and valid for as long as it is.
		 */
		const char* getUserAgent();
		uint32_t    getSlots();
		bool getHubCount(int& op, int& registered, int& regular);
		bool haveMode();
		bool isActiveMode();
		bool isPassiveMode();

	protected:
		void parse(std::string_view tag);
		bool parsePair(std::string_view name, std::string_view value);

	protected:
		/*
		 * A field left empty is a field the tag did not carry. None of these has
		 * an empty value that means anything, so the two cases do not need
		 * telling apart.
		 */
		bool ok;
		std::string client;
		std::string version;
		std::string ua;
		std::string mode;
		std::string hubcount;
		std::string slotcount;
};

}

#endif // HAVE_QUICKDC_DCTAG_H
