/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <samurai/io/file.h>

int main(int, char**) {
	Samurai::IO::File a("~/.jalla");					// expecting: /home/janvidar
	Samurai::IO::File b("~////.jalla/////");			// expecting: /home/janvidar/.jalla
	Samurai::IO::File c("//home/~////.jalla/////");	// expecting: /home/~/.jalla
	Samurai::IO::File d("/home/janvidar/../..");  // expecting: /
	Samurai::IO::File e("//home////.jalla////../janv/../janv/../janvidar/.quickdc//..");  // expecting: /home/janvidar
	Samurai::IO::File f("../../../../../../../home/");  // expecting: /home
	Samurai::IO::File g("../../../../../../../home/../");  // expecting: /
	Samurai::IO::File h("../../../../../../../home/.../");  // expecting: /home/...
	Samurai::IO::File i("/test/file.txt");
	printf("File extension: is '%s'\n", i.getExtension().c_str());
	printf("Match file extension: %s\n", i.matchExtension("TXT") ? "yes": "no");
}
