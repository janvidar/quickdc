/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_COMMANDS_H
#define HAVE_QUICKDC_CLI_COMMANDS_H

#include "script.h"

#include <string>
#include <vector>

namespace QuickDC::Cli {

/** One verb of the script language: how it is written, and how a line of it is read. */
struct Verb {
	const char* name;
	const char* usage;
	Step (*parse)(const Line& line);

	/** Run before the cores are built, and so only ahead of every other verb. */
	bool beforeStart = false;
};

const std::vector<Verb>& verbs();

/** The verb called 'name', or null. */
const Verb* findVerb(const std::string& name);

}

#endif // HAVE_QUICKDC_CLI_COMMANDS_H
