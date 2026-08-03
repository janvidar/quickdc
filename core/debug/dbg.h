/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef BUILD
#define BUILD "internal"
#endif

/*
 * NOTE: the macros below are #undef'd before being defined. samurai/debug/dbg.h
 * defines the same names pointing at samurai_debug() and friends, so merely
 * deferring to whoever got there first would make the log a translation unit
 * writes to depend on its include order. This header is QuickDC's, and
 * QuickDC's logging is what a QuickDC source file should get.
 */
#if defined(DEBUG)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef QDBG
#define QDBG(format, ...)  do { QuickDC_Debug(__PRETTY_FUNCTION__, __FILE__, __LINE__, format, ## __VA_ARGS__); } while(0)

#undef QERR
#define QERR(format, ...)  do { QuickDC_Error(__PRETTY_FUNCTION__, __FILE__, __LINE__, format, ## __VA_ARGS__); } while(0)

#undef QNET
#define QNET(format, ...)  do { QuickDC_Net  (__PRETTY_FUNCTION__, __FILE__, __LINE__, format, ## __VA_ARGS__); } while(0)

#undef QSEARCH
#define QSEARCH(format, ...)  do { QuickDC_Search(__PRETTY_FUNCTION__, __FILE__, __LINE__, format, ## __VA_ARGS__); } while(0)

#undef QHUB
#define QHUB(format, ...)  do { QuickDC_Hub(__PRETTY_FUNCTION__, __FILE__, __LINE__, format, ## __VA_ARGS__); } while(0)

#undef QDBG_INIT
#define QDBG_INIT do { QuickDC_Debug_Init(); } while(0)

#undef QDBG_FINI
#define QDBG_FINI do { QuickDC_Debug_Fini(); } while(0)

/*
 * The format attribute is what makes the compiler check these calls, as it
 * already does samurai's. Without it the macros above accept any argument for
 * any conversion: a std::string handed to %s, or a size_t to %d, compiles
 * silently and misreads whatever is on the stack. There are around four hundred
 * and fifty call sites in core/ and nothing covers the logging, so the check
 * has to come from the compiler.
 */
#if defined(__GNUC__) || defined(__clang__)
#define QUICKDC_PRINTF_LIKE(fmt_index, first_arg) \
	__attribute__((format(printf, fmt_index, first_arg)))
#else
#define QUICKDC_PRINTF_LIKE(fmt_index, first_arg)
#endif

void QuickDC_Debug_Init();
void QuickDC_Debug_Fini();

void QuickDC_Debug(const char* func, const char* file, int line, const char *format, ...) QUICKDC_PRINTF_LIKE(4, 5);
void QuickDC_Error(const char* func, const char* file, int line, const char *format, ...) QUICKDC_PRINTF_LIKE(4, 5);
void QuickDC_Net(const char* func, const char* file, int line, const char *format, ...) QUICKDC_PRINTF_LIKE(4, 5);
void QuickDC_Search(const char* func, const char* file, int line, const char *format, ...) QUICKDC_PRINTF_LIKE(4, 5);
void QuickDC_Hub(const char* func, const char* file, int line, const char *format, ...) QUICKDC_PRINTF_LIKE(4, 5);


#else /* ! DEBUG */

#include <stdio.h>

#undef QDBG
#define QDBG(format, ...) do { } while(0)

#undef QERR
#define QERR(format, ...) do { fprintf(stderr, "ERROR: "); fprintf(stderr, format, ## __VA_ARGS__); fprintf(stderr, "\n"); } while(0)

#undef QNET
#define QNET(format, ...) do { } while(0)

#undef QSEARCH
#define QSEARCH(format, ...)  do { } while(0)

#undef QHUB
#define QHUB(format, ...)  do { } while(0)

#undef QDBG_INIT
#define QDBG_INIT do { } while(0)

#undef QDBG_FINI
#define QDBG_FINI do { } while(0)


#endif // DEBUG

