/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_EVENTJSON_H
#define HAVE_QUICKDC_CLI_EVENTJSON_H

#include "json.h"

#include "api/protocol.h"

#include <functional>
#include <string>
#include <vector>

namespace QuickDC::Cli {

/** What a script calls the hub behind a handle. */
using HubNamer = std::function<Json::Value(QuickDC::Api::HubHandle)>;

/** The event as one object: "core", "event", then every field it carries. */
Json::Object describe(const std::string& core, const QuickDC::Api::Event& event,
                      const HubNamer& hubName);

const char* eventName(const QuickDC::Api::Event& event);

/** Every name eventName() can return, in the order of the variant. */
const std::vector<std::string>& eventNames();

}

#endif // HAVE_QUICKDC_CLI_EVENTJSON_H
