/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/http/request.h"
#include "network/http/header.h"
#include "network/http/entity.h"

Http::Request::Request()
	: Entity(true), method(UNKNOWN), protocol(HTTP_1_0)
{

}

Http::Request::Request(enum Http::Request::Method method_, const char* uri_, enum Http::Request::Protocol protocol_)
	: Entity(true), method(method_), protocol(protocol_), uri(uri_ ? uri_ : "")
{
}



void Http::Request::compose(QuickDC::IOBuffer*) {
	// FIXME: Implement me
}
