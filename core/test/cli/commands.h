/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_COMMANDS_H
#define HAVE_QUICKDC_CLI_COMMANDS_H

#include <string>
#include <vector>

/**
 * Handler for one command. The arguments have already been split and
 * validated against CommandData::args by the dispatcher, so a handler can
 * index the ones it declared as required without checking the size first.
 */
typedef void (*CommandFunc)(const std::vector<std::string>& args);

/**
 * One entry in the command table.
 *
 * 'args' is the single source of truth for a command's arguments: the
 * dispatcher uses it to split the input and to reject malformed invocations,
 * and /help uses it to generate the usage line. Keeping one description
 * instead of a separate parser and help text is what stops the two from
 * drifting apart.
 *
 * It is a space separated list of "type:name" tokens, where type is:
 *
 *   n   a nick (one word, resolved against the hub by the handler)
 *   w   any single word
 *   i   an integer (validated by the dispatcher)
 *   s   a string, consuming the rest of the line
 *
 * A token prefixed with '?' is optional; everything after an optional token
 * must also be optional. Since 's' consumes the rest of the line it may only
 * appear as the last token.
 *
 * A null handler marks a command that is listed but not implemented yet;
 * /help shows it as such rather than pretending it works.
 */
struct CommandData {
	const char* cmd;
	const char* args;
	const char* description;
	int         hidden;    /* only listed by `/help all' */
	CommandFunc handler;
};

extern void parseCommand(const char* line, size_t size);

/**
 * Record a search reply so that `/download <n>' can refer to it by number.
 * Prints the hit with its index as a side effect.
 *
 * This is what makes a download possible from the shell at all: queueing a
 * file needs its TTH and size, and a search reply is where those come from.
 */
extern void cmd_addSearchResult(const char* filename, uint64_t size,
	const char* tth, const char* nick, const char* cid);

#endif // HAVE_QUICKDC_CLI_COMMANDS_H
