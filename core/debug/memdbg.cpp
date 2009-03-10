/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifdef QUICKDC_MEMDBG

#include "quickdc.h"

extern "C" {
	/* forward declarations from malloc.cpp */
	void* dlmalloc(size_t);
	void dlfree(void*);
	void *dlrealloc(void*, size_t);
	void *dlcalloc(size_t, size_t);
}

#define FUNC_MALLOC  "MALLOC"
#define FUNC_FREE    "DELETE"
#define FUNC_CALLOC  "CALLOC"

// These are not used
#define FUNC_REAFREE "re-free"
#define FUNC_REALLOC "realloc"

extern "C" {


	void debug_malloc(void* addr, size_t size, void* code_addr, void* code_addr_up) {
		QuickDC_Memory(FUNC_MALLOC, addr, size, code_addr, code_addr_up);
	}
	
	void debug_free(void* addr, void* code_addr, void* code_addr_up) {
		if (addr) {
			QuickDC_Memory(FUNC_FREE, addr, 0, code_addr, code_addr_up);
		}
	}
	
	void debug_realloc(void* oldaddr, void* addr, size_t size, void* code_addr, void* code_addr_up) {
		// NOTE: A realloc is threated like a free + malloc.
		QuickDC_Memory(FUNC_FREE,   oldaddr, 0, code_addr, code_addr_up);
		QuickDC_Memory(FUNC_MALLOC, addr, size, code_addr, code_addr_up);
	}
	
	void debug_calloc(void* addr, size_t nmemb, size_t size, void* code_addr, void* code_addr_up) {
		QuickDC_Memory(FUNC_CALLOC, addr, size*nmemb, code_addr, code_addr_up);
	}
	
	void* malloc(size_t size)
	{
		void* addr = dlmalloc(size);
		debug_malloc(addr, size, __builtin_return_address(1), __builtin_return_address(2));
		return addr;
	}

	void free(void* addr)
	{
		debug_free(addr, __builtin_return_address(1), __builtin_return_address(2));
		dlfree(addr);
	}

	void* realloc(void* old, size_t size)
	{
		void* addr = dlrealloc(old, size);
		debug_realloc(old, addr, size, __builtin_return_address(1), __builtin_return_address(2));
		return addr;
	}

	void* calloc(size_t nmemb, size_t size)
	{
		void* addr = dlcalloc(nmemb, size);
		debug_calloc(addr, nmemb, size, __builtin_return_address(1), __builtin_return_address(2));
		return addr;
	}
}

#endif // QUICKDC_MEMDBG
