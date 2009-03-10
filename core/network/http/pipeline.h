/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_HTTP_PIPELINE_H
#define HAVE_QUICKDC_HTTP_PIPELINE_H

#include <vector>

namespace Http {

class Entity;

class Pipeline
{
	public:
		Pipeline();
		virtual ~Pipeline();

		/**
		 * Handle the request
		 * @return true if the request was successfully handled,
		 * or false if it could not be completed immediately.
		 */
		bool handle();
		
	public:
		void push(Http::Entity* entity);
		Http::Entity* pop();
		size_t size();
	
	protected:
		std::vector<Http::Entity*> queue;
};

}

#endif // HAVE_QUICKDC_HTTP_PIPELINE_H
