/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_DCTAG_H
#define HAVE_QUICKDC_DCTAG_H

#include "quickdc.h"

namespace DC {

class Tag
{
	public:
		Tag(const char* string);
		virtual ~Tag();
	
	public:
		bool isOK();
		
		const char* getUserAgent();
		uint32_t    getSlots();
		bool getHubCount(int& op, int& registered, int& regular);
		bool haveMode();
		bool isActiveMode();
		bool isPassiveMode();

	protected:
		void parse();
		bool parsePair(char*, char*);

	protected:
		bool ok;
		char* tag;
		char* client;
		char* version;
		char* ua;
		char* mode;
		char* hubcount;
		char* slotcount;
};

}

#endif // HAVE_QUICKDC_DCTAG_H
