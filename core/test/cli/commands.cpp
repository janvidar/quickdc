
/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <stdarg.h>

#include "quickdc.h"
#include "api/core.h"
#include "test/cli/hubconnection.h"
#include "test/cli/commands.h"
#include "test/cli/util.h"

#include "network/adc/server/adchub.h"
#include "network/adc/server/adchubuser.h"
#include <samurai/io/net/bandwidth.h>
#include "network/connection.h"
#include "network/connectionmanager.h"
#include "share/sharemanager.h"
#include "share/searchrequest.h"
#include "network/user.h"
#include "network/usermanager.h"
#include "network/transfermanager.h"
#include "network/transfer.h"
#include <samurai/io/file.h>
#include <samurai/io/net/url.h>

extern QuickDC::Core* core;
extern HubConnection* hub;
extern bool running;

/*

static const struct CommandData commands[] =
{
	{ "allow_connections", 17, 0, "",          "Toggle allow connections ON or OFF"            , 1 },
	{ "browse",      6, 1, "n",                "Browse user"                                   , 0 },
	{ "close",       5, 1, "i",                "Close transfer"                                , 1 },
	{ "connect",     7, 0, "?s",               "Connect to hub"                                , 0 },
	{ "connections",11, 0, "",                 "List all active p2p connections"               , 0 },
	{ "ctm",         3, 1, "n",                "Request connection to user"                    , 0 },
	{ "disconnect", 10, 0, "",                 "Disconnect from hub"                           , 0 },
	{ "download",    8, 2, "ns",               "Download file from user"                       , 0 },
	{ "help",        4, 0, "",                 "Show this cruft"                               , 0 },
	{ "hubstats",    8, 0, "",                 "Show hub statistics"                           , 0 },
	{ "hubban",      6, 1, "n",                "Ban user from local hub"                       , 1 },
	{ "hubunban",    8, 1, "n",                "Unban user from local hub"                     , 1 },
	{ "hubkick",     7, 1, "n",                "Kick user from local hub"                      , 1 },
	{ "info",        4, 1, "n",                "Show information about a given user"           , 0 },
	{ "joins",       5, 0, "",                 "Toggle showing join/leave messages"            , 0 },
	{ "kick",        4, 1, "n",                "Kick user from hub (operators only)"           , 0 },
	{ "list",        4, 0, "",                 "Show users"                                    , 0 },
	{ "msg",         3, 2, "ns" ,              "Send private message to user"                  , 0 },
	{ "quit",        4, 0, "",                 "Quit program"                                  , 0 },
	{ "redirect",    8, 2, "ns",               "Redirect user to another hub (operators only)" , 1 },
	{ "search",      6, 0, "s",                "Search for files"                              , 0 },
	{ "stats",       5, 0, "",                 "Show statistics"                               , 0 },
	{ "transfers",   9, 0, "",                 "Show active transfers"                         , 0 },
	{ "uptime",      6, 0, "",                 "Show QuickDC uptime information"               , 0 },
	{ "version",     7, 0, "",                 "Show software version"                         , 0 },
	{ 0, 0, 0, 0, 0, 1 }
};

*/

void cmd_output(const char* format, ...)
{
	char logmsg[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(logmsg, 1024, format, args);
	va_end(args);
	printf("*** %s\n", logmsg);
}

void cmd_error(const char* format, ...)
{
	char logmsg[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(logmsg, 1024, format, args);
	va_end(args);
	printf("*** ERROR: %s\n", logmsg);
}

ArgHandle::ArgHandle(char* s) {
	user = 0;
	if (hub && hub->getUserManager())
		user = const_cast<QuickDC::User*>(hub->getUserManager()->getUserByNick(s));
}

ArgHandle::~ArgHandle() {
	user = 0;
}

bool ArgHandle::isValid()
{
	return user;
}

ArgString::ArgString(char* s)
{
	string = strdup(s);
}

ArgString::~ArgString()
{
	free(string);
};

bool ArgString::isValid()
{
	return string;
}

ArgInteger::ArgInteger(char* s)
{
	valid = true;
	number = atoi(s);
	if (number == 0 && strcmp(s, "0")) valid = false;
}

ArgInteger::~ArgInteger()
{
}

bool ArgInteger::isValid()
{
	return valid;
}


Command::Command(const char* line_, size_t)
{
	line = strdup(line_);
	cmd = 0;
	valid = false;
	parse();
}

Command::~Command()
{
	free(cmd);
	
	while (arguments.size()) {
		Arg* a = arguments.back();
		arguments.pop_back();
		delete a;
	}
	
}

bool Command::parse()
{
	if (!line || !strlen(line)) return false;

	// separate command from arguments (if present).
	char* space = strchr(line, ' ');
	if (space) {
		if (space == line) return false;
		cmd = strndup(line, &space[0]-&line[0]);
	} else {
		cmd = strdup(line);
		return true;
	}
	
	std::vector<char*> args;
	
	// Now, split arguments in multiple strings
	char* oldpos = space;
	space = strchr(space, ' ');
	while (space)
	{
		args.push_back(strndup(oldpos+1, &space[0]-(&oldpos[0]+1)));
		oldpos = space;
		space = strchr(space, ' ');
	}

	if (strlen(oldpos))
		args.push_back(strdup(oldpos));
	
	
	/*
	// cmd_output("Command reference:");
	const struct CommandData* CMD = 0;
	for (size_t n = 0; commands[n].cmd; n++) {
		if (commands[n].length == strlen(cmd) && strcasecmp(commands[n].cmd, cmd) == 0)
		{
			CMD = &commands[n];
			break;
		}
	}
	
	if (!CMD) {
		return false;
	}
	
	return true;
	*/
	
	return false;
}

bool Command::isValid()
{
	return valid;
}

#define MATCH(x) strncmp(x, &cmd[1], strlen(x)) == 0

void parseCommand(const char* cmd, size_t size) {
//	printf("parseCommand(\"%s\", %d)\n", cmd, (int) size);
	
	if (MATCH("allow_connections")) {
		hub->allowConnections = !hub->allowConnections;
		cmd_output("Allow connections %s", (hub->allowConnections ? "ON" : "OFF"));

	} else if (MATCH("quit")) {
		running = false;
		

	} else if (MATCH("disconnect")) {
		hub->disconnect();
	} else if (MATCH("version")) {
		cmd_output(	"%s\n"
					"    Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org\n"
					"    This is free software with ABSOLUTELY NO WARRANTY.\n\n", core->getVersion());
			
	} else if (MATCH("help")) {
		cmd_output("Command reference:");
		/*
		for (size_t n = 0; commands[n].cmd; n++) {
			if (!commands[n].hidden)
				printf("    %15s %16s   - %s\n", commands[n].cmd, commands[n].args, commands[n].description);
		}
		*/
		
	} else if (MATCH("msg ")) {
		const char* split = strchr(&cmd[6], ' ');
		if (split) {
			char* nick = strndup(&cmd[5], &split[0]-&cmd[5]);
			const QuickDC::User* user = hub->getUserManager()->getUserByNick(nick);
			if (user) {
				const char* msg = &split[1];
				if (strlen(msg))
					hub->sendPrivateChatMessage(user, msg);
			} else {
				cmd_error("No user named '%s'", nick);
			}
			free(nick);
		}
	
	} else if (MATCH("ctm ")) {
		const char* nick = &cmd[5];
		const QuickDC::User* user = hub->getUserManager()->getUserByNick(nick);
		if (user) {
				hub->connectUser(user->getID());
		} else {
			cmd_error("*** No user named '%s'", nick);
		}
	
	} else if (MATCH("info ")) {
		const QuickDC::User* user = hub->getUserManager()->getUserByNick(&cmd[6]);
		if (user) {
			printf("*** User information for '%s'\n", &cmd[6]);
			printf("    Nick:            %s\n", user->getNick());
			printf("    Description:     %s\n", user->getDescription());
			printf("    E-mail:          %s\n", user->getEmail());
			printf("    Shared size:     %s\n", getFormatSize(user->getSharedBytes()));
			printf("    User-agent:      %s\n", user->getUserAgent());
			printf("    Stats:           %d slots, %d/%d/%d (op/reg/norm)\n",
					user->getNumSlots(),
					user->getHubsOperator(),
					user->getHubsRegistered(),
					user->getHubsRegular()
				);
		} else {
			cmd_error("*** No user named '%s'", &cmd[6]);
		}
	} else if (MATCH("join")) {
		hub->showjoins = !hub->showjoins;
		cmd_output("*** Hub joins are now %s", (hub->showjoins ? "ON" : "OFF"));

	} else if (MATCH("list")) {
		QuickDC::UserManager* users = hub->getUserManager();
		if (users) {
			printf("*** Users online: %d\n", (int) users->countUsers());
			QuickDC::User* u = users->first();
			while (u) {
				printf("  %32s  %16s  '%s'\n", u->getNick(), getFormatSize(u->getSharedBytes()), u->getDescription());

				u = users->next();
			}
		} else {
			cmd_error("*** Unable to obtain user list, not online");
		}

	} else if (MATCH("transfers")) {
		cmd_output("*** Transfers: %d\n", (int) core->transfers->size());
		QuickDC::Transfer* t = core->transfers->first();
		while (t) {
			printf("  %15s:%d\t%8d bps (%s) '%s'\n", t->getSocket()->getAddress()->toString(), t->getSocket()->getPort(), (int) t->getBps(), t->isDownload() ? "DN" : "UP", t->getFile()->getName().c_str());
			t = core->transfers->next();
		}

	} else if (MATCH("connections")) {
		cmd_output("*** Connections: %d\n", (int) core->connections->size());
		QuickDC::Connection* c = core->connections->first();
		while (c) {
			printf("  %s:%5d    %s\n", c->getSocket()->getAddress()->toString(), c->getSocket()->getPort(), c->getProtocol());
			c = core->connections->next();
		}
		
	} else if (MATCH("connect")) {
		hub->connect();
	} else if (MATCH("search ")) {
		if (size < 8) return;
		
		QuickDC::Share::SearchRequest* request = new QuickDC::Share::SearchRequest(QuickDC::Share::SearchRequest::SizeAny, 0, 0, "console", 0);
		char* term = strdup(&cmd[8]);
		char* start = term;
		char* end = strchr(start, ' ');
		while (end) {
			end[0] = '\0';
			
			if (start[0] == '+' && strlen(start) > 1)
				request->addInclusion(&start[1]);
			else if (start[0] == '-' && strlen(start) > 1)
				request->addExclusion(&start[1]);
			else
				request->addInclusion(start);

			start = &end[1];
			end = strchr(start, ' ');
		}
		
		if (start[0] == '+' && strlen(start) > 1)
			request->addInclusion(&start[1]);
		else if (start[0] == '-' && strlen(start) > 1)
			request->addExclusion(&start[1]);
		else
			request->addInclusion(start);
		
		hub->sendSearchRequest(request);
		
		delete request;
		free(term);
		
	} else if (MATCH("tthsearch ")) {
		if (size < 11) return;
		
		QuickDC::Share::SearchRequest* request = new QuickDC::Share::SearchRequest(QuickDC::Share::SearchRequest::SizeAny, 0, (char*) &cmd[11], "consoletth", 0);
		
		QuickDC::Core::getInstance()->shares->search(request);
		delete request;
	
	} else if (MATCH("browse ")) {
		if (size < 8) return;
		const QuickDC::User* user = hub->getUserManager()->getUserByNick(&cmd[8]);
		printf("Browse user '%s'\n", &cmd[8]);
		if (user) {
			hub->connectUser(user->getID());
		} else {
			printf("*** No user named '%s'\n", &cmd[8]);
		}
		
	} else if (MATCH("kick ")) {
		if (size < 6) return;
		const QuickDC::User* user = hub->getUserManager()->getUserByNick(&cmd[6]);
		if (user) {
			hub->sendKickUser(user->getID());
		} else {
			printf("*** No user named '%s'\n", &cmd[6]);
		}		
	
	} else if (MATCH("stats")) {
                QuickDC::UserManager* users = hub->getUserManager();
		if (users) {
			printf("*** Users online: %d\n", (int) users->countUsers());
		} else {
			printf("*** Unable to obtain user list\n");
		}

	} else if (MATCH("hubstats")) {
		ADC::Hub* localHub = QuickDC::Core::getInstance()->localHub;
		if (localHub) {
			printf("*** Local hub started. %d/%d user(s) online\n", 
				(int) localHub->users.size(),
				(int) localHub->maxUsers);

			int count_state_none     = 0;
			int count_state_protocol = 0;
			int count_state_identify = 0;
			int count_state_verify   = 0;
			int count_state_normal   = 0;
			int count_state_wait     = 0;
			int count_cred_user      = 0;
			int count_cred_operator  = 0;
			int count_cred_admin     = 0;
			size_t max_send_queue    = 0;
			size_t tot_send_queue    = 0;

			std::vector<ADC::HubUser*>::iterator it;
			for (it = localHub->users.begin(); it != localHub->users.end(); it++) {
				ADC::HubUser* user = (*it);
				switch (user->getCredentials()) {
					case ADC::Cred_User:            count_cred_user++;      break;
					case ADC::Cred_Operator:        count_cred_operator++;  break;
					case ADC::Cred_Admin:           count_cred_admin++;     break;
				}
				switch (user->getState()) {
					case ADC::State_None:           count_state_none++;     break;
					case ADC::State_Protocol:       count_state_protocol++; break;
					case ADC::State_Identify:       count_state_identify++; break;
					case ADC::State_Verify:         count_state_verify++;   break;
					case ADC::State_Normal:         count_state_normal++;   break;
					case ADC::State_WaitDisconnect: count_state_wait++;     break;
				}

				
				if (user->getSendQueueSize() > max_send_queue)
					max_send_queue = user->getSendQueueSize();
				
				tot_send_queue += user->getSendQueueSize();
				

			}
			
			printf("    User statistics:\n");
			printf("    - Users:         %d\n"
			       "    - Operators:     %d\n"
			       "    - Admins:        %d\n\n",
				count_cred_user,
				count_cred_operator,
				count_cred_admin
			);

			printf("    Connection states:\n");
			printf("    - Connecting:    %d\n"
			       "    - Normal:        %d\n"
                               "    - Disconnecting: %d\n\n",
				count_state_none + count_state_protocol + count_state_identify + count_state_verify,
				count_state_normal,
				count_state_wait);

			printf("    Send queue:\n"
			       "    - Max queue:     %d (bytes)\n"
			       "    - Total:         %d (bytes)\n\n",
				(int) max_send_queue,
				(int) tot_send_queue);

			printf("    Bandwidth utilization:\n"
			       "    - Upstream:      %d (bytes/sec)\n"
			       "    - Downstream:    %d (bytes/sec)\n\n",
				(int) core->bandwidthManager->getSendBps(),
				(int) core->bandwidthManager->getRecvBps());


		} else {
			printf("*** Local hub not started\n");
		}

		
	} else {
		printf("*** Unknown command: `%s'. Try `/help'\n", cmd);
	}
}
