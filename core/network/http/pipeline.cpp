/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <samurai/debug/dbg.h>
#include "network/http/pipeline.h"
#include "network/http/request.h"

Http::Pipeline::Pipeline() { }
Http::Pipeline::~Pipeline() { }


void Http::Pipeline::push(Http::Entity* msg)
{
	QDBG("Added entity to pipeline");
	queue.push_back(msg);
}

size_t Http::Pipeline::size()
{
	return queue.size();
}

bool Http::Pipeline::handle()
{
// 	Http::Entity* entity = queue.front();
	return false;
}


Http::Entity* Http::Pipeline::pop()
{
	QDBG("Removed entity from pipeline");
	if (!queue.size()) return 0;
	
	Http::Entity* entity = queue.front();
	queue.erase(queue.begin());
	
	return entity;
}
