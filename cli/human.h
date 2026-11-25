/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_HUMAN_H
#define HAVE_QUICKDC_CLI_HUMAN_H

#include "json.h"

#include <string>

namespace QuickDC::Cli {

/**
 * An event as a person reads it: the core, the event, then each field as the
 * path a wait would name it by and its value, the empty ones left out. Each item
 * of an array of objects goes on a line of its own.
 */
std::string readable(const Json::Object& event);

}

#endif // HAVE_QUICKDC_CLI_HUMAN_H
