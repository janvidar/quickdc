/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdio.h>
#include <string.h>
#include <string>
#include <samurai/stdc.h>
#include "network/dc/lock.h"

#include "network/dc/textescape.h"

int main(int , char** ) {
/*
	if (argc < 2) {
		printf("Usage: %s lock\n", argv[0]);
		return -1;
	}
	
	
	char* key = QuickDC::Lock::calculateKey(argv[1], strlen(argv[1]));
	printf("Key: '%s'\n", key);
*/	
	char* message = strdup("/home/janv&#36; apt-get update &amp;&amp; apt-get upgrade &#124; grep new");
	const char* msg2 = DC::TextEscape::unescape(message);
	printf("MESSAGE: '%s'\n", msg2);
}

// eof
