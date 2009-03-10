/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "debug/dbg.h"

#include "sys/io/file.h"

#define DUMP_FILE_NETWORK "~/.quickdc/debug/network.log"
#define DUMP_FILE_SEARCH  "~/.quickdc/debug/search.log"
#define DUMP_FILE_DEBUG   "~/.quickdc/debug/debug.log"
#define DUMP_FILE_HUB     "~/.quickdc/debug/hub.log"
#define DUMP_FILE_MEM     "~/.quickdc/debug/memory.log"

#define PREFIX "../"

#ifdef WIN32
#define ENDLINE "\r\n"
#else
#define ENDLINE "\n"
#endif

#define DUPLICATE_TO_DEBUG


static System::IO::File* quickdc_dump_net;
static System::IO::File* quickdc_dump_sch;
static System::IO::File* quickdc_dump_dbg;
static System::IO::File* quickdc_dump_hub;
#ifdef QUICKDC_MEMDBG
static System::IO::File* quickdc_dump_mem;
#endif


void QuickDC_Debug_Init() {
	fprintf(stderr, "%s starting up...\n", PRODUCT " " VERSION " (build " BUILD ") - debug code enabled");
	
	System::IO::File::mkdir("~/.quickdc",       0700);
	System::IO::File::mkdir("~/.quickdc/debug",   0700);
	
#ifdef QUICKDC_MEMDBG
	quickdc_dump_mem = new System::IO::File(DUMP_FILE_MEM);
	quickdc_dump_mem->open(System::IO::File::MODEWO, true);
#endif
	
	quickdc_dump_net = new System::IO::File(DUMP_FILE_NETWORK);
	quickdc_dump_sch = new System::IO::File(DUMP_FILE_SEARCH);
	quickdc_dump_dbg = new System::IO::File(DUMP_FILE_DEBUG);
	quickdc_dump_hub = new System::IO::File(DUMP_FILE_HUB);
	quickdc_dump_net->open(System::IO::File::MODEWO, true);
	quickdc_dump_sch->open(System::IO::File::MODEAPPEND);
	quickdc_dump_dbg->open(System::IO::File::MODEWO, true);
	quickdc_dump_hub->open(System::IO::File::MODEAPPEND);
	
}

void QuickDC_Debug_Fini() {
	delete quickdc_dump_sch; quickdc_dump_sch = 0;
	delete quickdc_dump_hub; quickdc_dump_hub = 0;
	delete quickdc_dump_dbg; quickdc_dump_dbg = 0;
	delete quickdc_dump_net; quickdc_dump_net = 0;
#ifdef QUICKDC_MEMDBG
	delete quickdc_dump_mem; quickdc_dump_mem = 0;
#endif

}

void QuickDC_Debug(const char* /*func*/, const char* file, int line, const char *format, ...) {
	char logmsg[1024];
	char location[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(logmsg, 1024, format, args);
	va_end(args);
	
	// Strip the boring path details from the filename
	const char *separator = PREFIX;
	const char *shortfile = strstr(file, separator);
	shortfile = (shortfile ? shortfile + strlen(separator) + 1 : file);
	snprintf(location, 1024, "%32s:%u\t ", shortfile, line);
	
	if (!quickdc_dump_dbg)
	{
		fprintf(stderr, "DEBUG: %s: %s\n", location, logmsg);
		return;
	}
	quickdc_dump_dbg->write(logmsg, strlen(logmsg));
	quickdc_dump_dbg->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
	quickdc_dump_dbg->flush();
#endif
}

void QuickDC_Error(const char* /*func*/, const char* file, int line, const char *format, ...) {
	char logmsg[1024];
	char location[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(logmsg, 1024, format, args);
	va_end(args);
	
	// Strip the boring path details from the filename
	const char *separator = PREFIX;
	const char *shortfile = strstr(file, separator);
	shortfile = (shortfile ? shortfile + strlen(separator) + 1 : file);
	snprintf(location, 1024, "%32s:%u\t ", shortfile, line);
	fprintf(stderr, "ERROR: %s: %s\n", location, logmsg);
	
	if (!quickdc_dump_dbg) return;
	quickdc_dump_dbg->write("ERROR: ", strlen("ERROR: "));
	quickdc_dump_dbg->write(location, strlen(location));
	quickdc_dump_dbg->write(": ", strlen(": "));
	quickdc_dump_dbg->write(logmsg, strlen(logmsg));
	quickdc_dump_dbg->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
	quickdc_dump_dbg->flush();
#endif
}

void QuickDC_Net(const char*, const char* , int, const char *format, ...) {
	if (!quickdc_dump_net) return;
	
	char logmsg[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(logmsg, 1024, format, args);
	va_end(args);
	
	quickdc_dump_net->write(logmsg, strlen(logmsg));
	quickdc_dump_net->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
	quickdc_dump_net->flush();
#endif

#ifdef DUPLICATE_TO_DEBUG
        if (!quickdc_dump_dbg) return;
        quickdc_dump_dbg->write(logmsg, strlen(logmsg));
        quickdc_dump_dbg->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
        quickdc_dump_dbg->flush();
#endif
#endif

}

void QuickDC_Search(const char*, const char* , int, const char *format, ...) {
	if (!quickdc_dump_sch) return;
	
	char logmsg[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(logmsg, 1024, format, args);
	va_end(args);
	
	quickdc_dump_sch->write(logmsg, strlen(logmsg));
	quickdc_dump_sch->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
	quickdc_dump_sch->flush();
#endif

#ifdef DUPLICATE_TO_DEBUG
        if (!quickdc_dump_dbg) return;
        quickdc_dump_dbg->write(logmsg, strlen(logmsg));
        quickdc_dump_dbg->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
        quickdc_dump_dbg->flush();
#endif
#endif


}

void QuickDC_Hub(const char*, const char* , int, const char *format, ...) {
	if (!quickdc_dump_hub) return;
	
	char logmsg[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(logmsg, 1024, format, args);
	va_end(args);
	
	quickdc_dump_hub->write(logmsg, strlen(logmsg));
	quickdc_dump_hub->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
	quickdc_dump_hub->flush();
#endif
#ifdef DUPLICATE_TO_DEBUG
        if (!quickdc_dump_dbg) return;
        quickdc_dump_dbg->write(logmsg, strlen(logmsg));
        quickdc_dump_dbg->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
        quickdc_dump_dbg->flush();
#endif
#endif


}

#ifdef QUICKDC_MEMDBG
void QuickDC_Memory(const char* func, void* addr, size_t size, void* code_addr, void* code_addr_up) {
	if (!quickdc_dump_mem) return;
	
	char logmsg[1024] = {0, };
	sprintf(logmsg, "%s: addr=%p, size=%u, stack=%p, stack=%p", func, addr, size, code_addr, code_addr_up);
	quickdc_dump_mem->write(logmsg, strlen(logmsg));
	quickdc_dump_mem->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
	quickdc_dump_mem->flush();
#endif

#ifdef DUPLICATE_TO_DEBUG
        if (!quickdc_dump_dbg) return;
        quickdc_dump_dbg->write(logmsg, strlen(logmsg));
        quickdc_dump_dbg->write(ENDLINE, strlen(ENDLINE));
#ifdef FLUSH_DEBUG
        quickdc_dump_dbg->flush();
#endif
#endif

}
#endif



