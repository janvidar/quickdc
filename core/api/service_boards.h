/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_API_SERVICE_BOARDS_H
#define HAVE_QUICKDC_API_SERVICE_BOARDS_H

/* Private to the service; see service_boards.cpp. */

#include "api/service.h"
#include "bbs/boardindex.h"
#include "bbs/poststore.h"

#include <string>

/** Where one front end keeps the boards it follows and the posts it has read. */
class QuickDC::Api::Service::BoardStore final {
	public:
		BoardStore(const std::string& boards, const std::string& documents,
		           Share::Manager* shares)
			: index(boards)
			, posts(documents, shares)
		{
		}

		BoardStore(const BoardStore&) = delete;
		BoardStore& operator=(const BoardStore&) = delete;

		BoardIndex index;
		PostStore posts;
};

#endif // HAVE_QUICKDC_API_SERVICE_BOARDS_H
