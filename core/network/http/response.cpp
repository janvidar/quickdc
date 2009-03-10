/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/http/response.h"

Http::Response::Response() : Entity(false), code(0), message(0) { }
Http::Response::Response(uint16_t code_, const char* message_) : Entity(false), code(code_), message(0)
{
	message = strdup(message_);
}

Http::Response::~Response() {
	free(message);
}



