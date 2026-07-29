
/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <samurai/util/format.h>
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
#include "network/downloadqueue.h"
#include "network/filesource.h"
#include "config/preferences.h"
#include "api/hub.h"
#include <samurai/io/file.h>
#include <samurai/io/net/url.h>

extern QuickDC::Core* core;
extern HubConnection* hub;
extern bool running;


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


/* ------------------------------------------------------------------------ *
 * Argument specifications
 * ------------------------------------------------------------------------ */

struct ArgSpec {
	char        type;
	std::string name;
	bool        optional;
};

static std::vector<ArgSpec> parseArgSpec(const char* spec)
{
	std::vector<ArgSpec> out;
	if (!spec) return out;

	for (const char* p = spec; *p; )
	{
		while (*p == ' ') p++;
		if (!*p) break;

		ArgSpec arg;
		arg.optional = false;
		if (*p == '?') { arg.optional = true; p++; }
		if (!*p) break;

		arg.type = *p++;
		if (*p == ':') p++;

		const char* start = p;
		while (*p && *p != ' ') p++;
		arg.name.assign(start, p - start);

		out.push_back(arg);
	}
	return out;
}

/**
 * Build the usage line shown by /help and by argument errors, for example
 * "/msg <nick> <message>".
 */
static std::string usageString(const struct CommandData* command)
{
	std::string usage("/");
	usage += command->cmd;

	std::vector<ArgSpec> spec = parseArgSpec(command->args);
	for (size_t n = 0; n < spec.size(); n++)
	{
		usage += spec[n].optional ? " [" : " <";
		usage += spec[n].name;
		usage += spec[n].optional ? "]" : ">";
	}
	return usage;
}

static bool isInteger(const std::string& s)
{
	size_t n = (s.size() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
	if (n >= s.size()) return false;
	for (; n < s.size(); n++)
		if (!isdigit((unsigned char) s[n])) return false;
	return true;
}

/**
 * Split 'rest' (everything after the command word) according to the command's
 * argument specification. Reports the problem and returns false if the input
 * does not match, so the caller can simply bail out.
 */
static bool splitArguments(const struct CommandData* command, const char* rest, std::vector<std::string>& out)
{
	std::vector<ArgSpec> spec = parseArgSpec(command->args);
	const char* p = rest;

	for (size_t n = 0; n < spec.size(); n++)
	{
		while (*p == ' ') p++;

		if (!*p)
		{
			/* Optional arguments only ever trail, so we are done. */
			if (spec[n].optional) return true;

			cmd_error("Missing argument <%s>. Usage: %s", spec[n].name.c_str(), usageString(command).c_str());
			return false;
		}

		if (spec[n].type == 's')
		{
			out.push_back(std::string(p));
			p += strlen(p);
		}
		else
		{
			const char* start = p;
			while (*p && *p != ' ') p++;
			out.push_back(std::string(start, p - start));
		}

		if (spec[n].type == 'i' && !isInteger(out.back()))
		{
			cmd_error("Argument <%s> must be a number, got `%s'. Usage: %s",
				spec[n].name.c_str(), out.back().c_str(), usageString(command).c_str());
			return false;
		}
	}

	while (*p == ' ') p++;
	if (*p)
	{
		cmd_error("Too many arguments. Usage: %s", usageString(command).c_str());
		return false;
	}
	return true;
}


/* ------------------------------------------------------------------------ *
 * Shared helpers
 * ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ *
 * Search results
 *
 * Queueing a download needs a TTH and a size, and a search reply is the only
 * place the shell gets them. Replies are kept here so that `/download <n>'
 * can name one by number instead of the user retyping a 39 character hash.
 * ------------------------------------------------------------------------ */

#define MAX_SEARCH_HITS 200

struct SearchHit {
	std::string filename;
	std::string tth;
	std::string nick;
	std::string cid;
	uint64_t size;
};

static std::vector<SearchHit> searchHits;

/* Which search the listed results belong to. A slow peer can answer long after
   the next search was started, and those results must not be numbered in with
   the current ones. */
static std::string searchToken;

/**
 * Begin a new search, discarding the results of the previous one.
 * @return the token to send with it.
 */
static const char* cmd_newSearchToken()
{
	static unsigned long counter = 0;

	char buf[32];
	snprintf(buf, sizeof(buf), "cli%lu", ++counter);

	searchToken = buf;
	searchHits.clear();
	return searchToken.c_str();
}

void cmd_addSearchResult(const char* filename, uint64_t size,
	const char* tth, const char* nick, const char* cid, const char* token)
{
	if (!filename) return;

	/* No token means the sender did not echo ours back; take those rather than
	   dropping results from peers that leave it out. */
	if (token && *token && searchToken != token)
	{
		QDBG("Ignoring a result for search '%s'; we are showing '%s'",
			token, searchToken.c_str());
		return;
	}

	SearchHit hit;
	hit.filename = filename;
	hit.tth = (tth) ? tth : "";
	hit.nick = (nick) ? nick : "";
	hit.size = size;

	/*
	 * A FileSource is keyed on the CID for ADC and on the nick for NMDC, which
	 * has no CID at all. Fall back rather than storing an empty identifier.
	 */
	if (cid && *cid)
		hit.cid = cid;
	else
		hit.cid = hit.nick;

	/* Keep the numbering stable within a search; just stop growing when full. */
	if (searchHits.size() >= MAX_SEARCH_HITS)
	{
		cmd_error("Too many search results; ignoring the rest");
		return;
	}

	searchHits.push_back(hit);

	printf("  %3d  %10s  %-24s %s%s\n",
		(int) searchHits.size(),
		Samurai::Util::formatSize(size).c_str(),
		hit.nick.c_str(),
		filename,
		hit.tth.empty() ? "  (no TTH, cannot be queued)" : "");
}

/**
 * The last path component, which is what the file is called locally.
 * Remote lists use '/' but NMDC peers use '\', so both count.
 */
static std::string baseName(const std::string& path)
{
	size_t cut = path.find_last_of("/\\");
	if (cut == std::string::npos) return path;
	return path.substr(cut + 1);
}

/**
 * Where finished downloads go. Configurable, because ~/.quickdc is a poor
 * place to leave large files.
 */
static std::string downloadDirectory()
{
	QuickDC::Preferences* config = core->config;
	config->setGroup("Downloads");
	return std::string(config->getString("Directory", QUICKDC_DOWNLOAD_DIR));
}


/**
 * Resolve a nick against the connected hub, reporting why it failed.
 */
static const QuickDC::User* resolveUser(const std::string& nick)
{
	QuickDC::UserManager* users = hub->getUserManager();
	if (!users)
	{
		cmd_error("Not connected to a hub");
		return 0;
	}

	const QuickDC::User* user = users->getUserByNick(nick.c_str());
	if (!user)
		cmd_error("No user named `%s'", nick.c_str());

	return user;
}


/* ------------------------------------------------------------------------ *
 * The command table
 *
 * Kept sorted by name; /help prints it in this order. Commands with a null
 * handler have no implementation behind them yet - they are listed so the
 * gap stays visible rather than silently missing.
 * ------------------------------------------------------------------------ */

static void cmd_allow_connections(const std::vector<std::string>&);
static void cmd_browse(const std::vector<std::string>&);
static void cmd_connect(const std::vector<std::string>&);
static void cmd_connections(const std::vector<std::string>&);
static void cmd_ctm(const std::vector<std::string>&);
static void cmd_disconnect(const std::vector<std::string>&);
static void cmd_download(const std::vector<std::string>&);
static void cmd_help(const std::vector<std::string>&);
static void cmd_hubkick(const std::vector<std::string>&);
static void cmd_hubstats(const std::vector<std::string>&);
static void cmd_info(const std::vector<std::string>&);
static void cmd_joins(const std::vector<std::string>&);
static void cmd_kick(const std::vector<std::string>&);
static void cmd_list(const std::vector<std::string>&);
static void cmd_msg(const std::vector<std::string>&);
static void cmd_password(const std::vector<std::string>&);
static void cmd_queue(const std::vector<std::string>&);
static void cmd_quit(const std::vector<std::string>&);
static void cmd_redirect(const std::vector<std::string>&);
static void cmd_search(const std::vector<std::string>&);
static void cmd_stats(const std::vector<std::string>&);
static void cmd_transfers(const std::vector<std::string>&);
static void cmd_tthsearch(const std::vector<std::string>&);
static void cmd_uptime(const std::vector<std::string>&);
static void cmd_version(const std::vector<std::string>&);

static const struct CommandData commands[] =
{
	{ "allow_connections", "",                           "Toggle accepting incoming client connections",     1, cmd_allow_connections },
	{ "browse",            "n:nick",                     "Queue a user's file list for download",            0, cmd_browse            },
	{ "close",             "i:transfer",                 "Close a transfer",                                 1, 0                     },
	{ "connect",           "",                           "Connect to the hub",                               0, cmd_connect           },
	{ "connections",       "",                           "List all active p2p connections",                  0, cmd_connections       },
	{ "ctm",               "n:nick",                     "Request a connection to a user",                   0, cmd_ctm               },
	{ "disconnect",        "",                           "Disconnect from the hub",                          0, cmd_disconnect        },
	{ "download",          "i:result",                   "Queue search result <n> for download",             0, cmd_download          },
	{ "help",              "?s:command",                 "Show this command reference",                      0, cmd_help              },
	{ "hubban",            "n:nick",                     "Ban a user from the local hub",                    1, 0                     },
	{ "hubkick",           "n:nick",                     "Kick a user from the local hub",                   1, cmd_hubkick           },
	{ "hubstats",          "",                           "Show local hub statistics",                        0, cmd_hubstats          },
	{ "hubunban",          "n:nick",                     "Unban a user from the local hub",                  1, 0                     },
	{ "info",              "n:nick",                     "Show information about a given user",              0, cmd_info              },
	{ "joins",             "",                           "Toggle showing join/leave messages",               0, cmd_joins             },
	{ "kick",              "n:nick",                     "Kick a user from the hub (operators only)",        0, cmd_kick              },
	{ "list",              "",                           "Show users on the hub",                            0, cmd_list              },
	{ "msg",               "n:nick s:message",           "Send a private message to a user",                 0, cmd_msg               },
	{ "password",          "s:password",                 "Send a password to a hub that asked for one",      0, cmd_password          },
	{ "queue",             "",                           "Show the download queue",                          0, cmd_queue             },
	{ "quit",              "",                           "Quit the program",                                 0, cmd_quit              },
	{ "redirect",          "n:nick w:address ?s:reason", "Redirect a user to another hub (operators only)",  1, cmd_redirect          },
	{ "search",            "s:terms",                    "Search the hub for files (+term, -term)",          0, cmd_search            },
	{ "stats",             "",                           "Show hub statistics",                              0, cmd_stats             },
	{ "transfers",         "",                           "Show active transfers",                            0, cmd_transfers         },
	{ "tthsearch",         "s:tth",                      "Search the local share for a TTH root",            0, cmd_tthsearch         },
	{ "uptime",            "",                           "Show QuickDC uptime information",                  0, cmd_uptime            },
	{ "version",           "",                           "Show software version",                            0, cmd_version           },
	{ 0, 0, 0, 1, 0 }
};

static const struct CommandData* findCommand(const char* name)
{
	for (size_t n = 0; commands[n].cmd; n++)
		if (strcasecmp(commands[n].cmd, name) == 0)
			return &commands[n];
	return 0;
}


/* ------------------------------------------------------------------------ *
 * Command handlers
 * ------------------------------------------------------------------------ */

static void cmd_help(const std::vector<std::string>& args)
{
	bool all = args.size() && strcasecmp(args[0].c_str(), "all") == 0;

	/* /help <command> - describe a single command. */
	if (args.size() && !all)
	{
		const char* name = args[0].c_str();
		if (name[0] == '/') name++;

		const struct CommandData* command = findCommand(name);
		if (!command)
		{
			cmd_error("Unknown command: `%s'. Try `/help'", args[0].c_str());
			return;
		}

		cmd_output("%s", usageString(command).c_str());
		printf("    %s%s\n", command->description, command->handler ? "" : " (not implemented)");
		return;
	}

	cmd_output("Command reference:");
	for (size_t n = 0; commands[n].cmd; n++)
	{
		if (commands[n].hidden && !all) continue;
		printf("  %-38s %s%s\n",
			usageString(&commands[n]).c_str(),
			commands[n].description,
			commands[n].handler ? "" : " (not implemented)");
	}

	if (!all)
		printf("\n  `/help all' also lists advanced commands, `/help <command>' describes one.\n");
}

static void cmd_version(const std::vector<std::string>&)
{
	cmd_output(	"%s\n"
				"    Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org\n"
				"    This is free software with ABSOLUTELY NO WARRANTY.\n", core->getVersion());
}

static void cmd_uptime(const std::vector<std::string>&)
{
	time_t secs = core->startup.elapsed();
	int days  = (int)  (secs / 86400);
	int hours = (int) ((secs / 3600) % 24);
	int mins  = (int) ((secs / 60) % 60);
	int rest  = (int)  (secs % 60);

	cmd_output("Uptime: %d day%s, %02d:%02d:%02d", days, (days == 1) ? "" : "s", hours, mins, rest);
}

static void cmd_quit(const std::vector<std::string>&)
{
	running = false;
}

static void cmd_connect(const std::vector<std::string>&)
{
	hub->connect();
}

static void cmd_disconnect(const std::vector<std::string>&)
{
	hub->disconnect();
}

static void cmd_password(const std::vector<std::string>& args)
{
	hub->sendPassword(args[0].c_str());
}

static void cmd_allow_connections(const std::vector<std::string>&)
{
	hub->allowConnections = !hub->allowConnections;
	cmd_output("Allow connections %s", (hub->allowConnections ? "ON" : "OFF"));
}

static void cmd_joins(const std::vector<std::string>&)
{
	hub->showjoins = !hub->showjoins;
	cmd_output("Hub joins are now %s", (hub->showjoins ? "ON" : "OFF"));
}

static void cmd_list(const std::vector<std::string>&)
{
	QuickDC::UserManager* users = hub->getUserManager();
	if (!users)
	{
		cmd_error("Unable to obtain user list, not online");
		return;
	}

	cmd_output("Users online: %d", (int) users->countUsers());
	QuickDC::User* u = users->first();
	while (u)
	{
		printf("  %32s  %16s  '%s'\n", u->getNick(), Samurai::Util::formatSize(u->getSharedBytes()).c_str(), u->getDescription());
		u = users->next();
	}
}

static void cmd_stats(const std::vector<std::string>&)
{
	QuickDC::UserManager* users = hub->getUserManager();
	if (!users)
	{
		cmd_error("Unable to obtain user list, not online");
		return;
	}
	cmd_output("Users online: %d", (int) users->countUsers());
}

static void cmd_info(const std::vector<std::string>& args)
{
	const QuickDC::User* user = resolveUser(args[0]);
	if (!user) return;

	cmd_output("User information for '%s'", user->getNick());
	printf("    Nick:            %s\n", user->getNick());
	printf("    Description:     %s\n", user->getDescription());
	printf("    E-mail:          %s\n", user->getEmail());
	printf("    Shared size:     %s\n", Samurai::Util::formatSize(user->getSharedBytes()).c_str());
	printf("    User-agent:      %s\n", user->getUserAgent());
	printf("    Keyprint:        %s\n", *user->getKeyprint() ? user->getKeyprint() : "(none published)");
	printf("    Stats:           %d slots, %d/%d/%d (op/reg/norm)\n",
			user->getNumSlots(),
			user->getHubsOperator(),
			user->getHubsRegistered(),
			user->getHubsRegular()
		);
}

static void cmd_msg(const std::vector<std::string>& args)
{
	const QuickDC::User* user = resolveUser(args[0]);
	if (!user) return;
	hub->sendPrivateChatMessage(user, args[1].c_str());
}

static void cmd_ctm(const std::vector<std::string>& args)
{
	const QuickDC::User* user = resolveUser(args[0]);
	if (!user) return;
	hub->connectUser(user->getID());
}

static void cmd_browse(const std::vector<std::string>& args)
{
	const QuickDC::User* user = resolveUser(args[0]);
	if (!user) return;

	const char* hubUrl = hub->getUrl();

	/* The CID on ADC, the nick on NMDC. See filesource.h. */
	const char* cid = (const char*) user->getClientID();
	if (!cid || !*cid) cid = user->getNick();

	if (!hubUrl || !cid || !*cid)
	{
		cmd_error("Cannot browse `%s': no identifier is known for them", args[0].c_str());
		return;
	}

	/*
	 * Queue it, then run the queue so the request goes out now rather than on
	 * the next tick of the message loop. Note this never lands in queue.xml -
	 * a file list has no TTH and nothing to resume.
	 */
	core->queue->requestFilelist(hubUrl, cid);
	core->queue->process();

	cmd_output("Queued a file list request for '%s'. Use /queue to watch it.",
		user->getNick());
}

static void cmd_download(const std::vector<std::string>& args)
{
	int index = atoi(args[0].c_str());
	if (index < 1 || (size_t) index > searchHits.size())
	{
		if (searchHits.empty())
			cmd_error("No search results yet; try /search first");
		else
			cmd_error("No result number %d; there are %d", index, (int) searchHits.size());
		return;
	}

	const SearchHit& hit = searchHits[index - 1];

	if (hit.tth.empty())
	{
		cmd_error("That result carried no TTH, so it cannot be queued");
		return;
	}
	if (!hit.size)
	{
		cmd_error("That result carried no size, so it cannot be queued");
		return;
	}

	std::string target = downloadDirectory();
	target += "/";
	target += baseName(hit.filename);

	QuickDC::QueueFile* qfile = core->queue->add(target.c_str(), hit.tth.c_str(), hit.size);
	if (!qfile)
	{
		cmd_error("Unable to queue `%s'", target.c_str());
		return;
	}

	const char* hubUrl = hub->getUrl();
	if (hubUrl && !hit.cid.empty())
	{
		/* adc://host:port/{cid}/TTH:{root} - see filesource.h */
		std::string source("TTH:");
		source += hit.tth;
		qfile->addSource(new QuickDC::FileSource(hubUrl, hit.cid.c_str(), source.c_str()));
	}

	/* Start it now rather than on the next tick of the message loop. */
	core->queue->process();

	cmd_output("Queued `%s' (%s, %d source(s)).",
		target.c_str(), Samurai::Util::formatSize(hit.size).c_str(), (int) qfile->countSources());
}

static void cmd_queue(const std::vector<std::string>&)
{
	std::vector<QuickDC::QueueStatus> status;
	core->queue->getStatus(status);

	cmd_output("Queued files: %d (%d transfer(s) running)",
		(int) status.size(), (int) core->queue->countActiveTransfers());

	for (size_t n = 0; n < status.size(); n++)
	{
		const QuickDC::QueueStatus& item = status[n];

		int percent = 0;
		if (item.size) percent = (int) ((item.done * 100) / item.size);

		printf("  %3d%%  %10s  %s  src %d/%d  %s\n",
			percent,
			Samurai::Util::formatSize(item.size).c_str(),
			QuickDC::QueueFile::convertPriority(item.priority),
			(int) item.activeSources,
			(int) item.sources,
			item.filename);
	}

	size_t lists = core->queue->countFilelistRequests();
	if (!lists) return;

	cmd_output("File lists: %d", (int) lists);
	for (size_t n = 0; n < lists; n++)
	{
		QuickDC::FilelistRequest* list = core->queue->getFilelistRequest(n);
		if (!list) continue;

		printf("  %-9s  %s\n",
			list->isActive() ? "receiving" : "waiting",
			list->getClientID());
	}
}

static void cmd_kick(const std::vector<std::string>& args)
{
	const QuickDC::User* user = resolveUser(args[0]);
	if (!user) return;
	hub->sendKickUser(user->getID());
}

static void cmd_redirect(const std::vector<std::string>& args)
{
	const QuickDC::User* user = resolveUser(args[0]);
	if (!user) return;

	const char* reason = (args.size() > 2) ? args[2].c_str() : "Redirected by operator";
	hub->sendRedirect(user->getID(), args[1].c_str(), reason);
}

static void cmd_search(const std::vector<std::string>& args)
{
	QuickDC::Share::SearchRequest* request =
		new QuickDC::Share::SearchRequest(QuickDC::Share::SearchRequest::SizeAny, 0, 0,
			cmd_newSearchToken(), 0);

	char* terms = strdup(args[0].c_str());
	char* start = terms;
	for (;;)
	{
		char* end = strchr(start, ' ');
		if (end) end[0] = '\0';

		if (start[0] == '+' && strlen(start) > 1)
			request->addInclusion(&start[1]);
		else if (start[0] == '-' && strlen(start) > 1)
			request->addExclusion(&start[1]);
		else if (strlen(start))
			request->addInclusion(start);

		if (!end) break;
		start = &end[1];
	}

	hub->sendSearchRequest(request);

	delete request;
	free(terms);
}

static void cmd_tthsearch(const std::vector<std::string>& args)
{
	/* SearchRequest silently ignores anything that is not a TTH root, so check
	   here to be able to say why nothing happened. */
	if (args[0].size() != 39)
	{
		cmd_error("A TTH root is 39 characters, `%s' is %d", args[0].c_str(), (int) args[0].size());
		return;
	}

	char* tth = strdup(args[0].c_str());
	QuickDC::Share::SearchRequest* request =
		new QuickDC::Share::SearchRequest(QuickDC::Share::SearchRequest::SizeAny, 0, tth, "consoletth", 0);

	QuickDC::Core::getInstance()->shares->search(request);

	delete request;
	free(tth);
}

static void cmd_transfers(const std::vector<std::string>&)
{
	cmd_output("Transfers: %d", (int) core->transfers->size());
	QuickDC::Transfer* t = core->transfers->first();
	while (t)
	{
		printf("  %15s:%d\t%8d bps (%s) '%s'\n",
			t->getSocket()->getAddress()->toString().c_str(),
			t->getSocket()->getPort(),
			(int) t->getBps(),
			t->isDownload() ? "DN" : "UP",
			t->getFile()->getName().c_str());
		t = core->transfers->next();
	}
}

static void cmd_connections(const std::vector<std::string>&)
{
	cmd_output("Connections: %d", (int) core->connections->size());
	QuickDC::Connection* c = core->connections->first();
	while (c)
	{
		printf("  %s:%5d    %s\n",
			c->getSocket()->getAddress()->toString().c_str(),
			c->getSocket()->getPort(),
			c->getProtocol());
		c = core->connections->next();
	}
}

static void cmd_hubkick(const std::vector<std::string>& args)
{
	ADC::Hub* localHub = QuickDC::Core::getInstance()->localHub;
	if (!localHub)
	{
		cmd_error("Local hub not started");
		return;
	}

	ADC::HubUser* user = localHub->lookupUser(args[0].c_str());
	if (!user)
	{
		cmd_error("No user named `%s' on the local hub", args[0].c_str());
		return;
	}

	localHub->kick(user);
	cmd_output("Kicked `%s' from the local hub", args[0].c_str());
}

static void cmd_hubstats(const std::vector<std::string>&)
{
	ADC::Hub* localHub = QuickDC::Core::getInstance()->localHub;
	if (!localHub)
	{
		cmd_error("Local hub not started");
		return;
	}

	cmd_output("Local hub started. %d/%d user(s) online",
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
	for (it = localHub->users.begin(); it != localHub->users.end(); it++)
	{
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
}


/* ------------------------------------------------------------------------ *
 * Dispatch
 * ------------------------------------------------------------------------ */

/**
 * Look up a command, accepting any unambiguous abbreviation. An exact match
 * always wins, so a command that is a prefix of another (/connect against
 * /connections) stays reachable by its full name. Sets 'reported' when it has
 * already explained the failure to the user.
 */
static const struct CommandData* matchCommand(const char* name, bool& reported)
{
	reported = false;

	const struct CommandData* exact = findCommand(name);
	if (exact) return exact;

	size_t len = strlen(name);
	const struct CommandData* found = 0;
	size_t matches = 0;

	for (size_t n = 0; commands[n].cmd; n++)
	{
		if (strncasecmp(commands[n].cmd, name, len) != 0) continue;
		found = &commands[n];
		matches++;
	}

	if (matches == 1) return found;

	if (matches > 1)
	{
		std::string list;
		for (size_t n = 0; commands[n].cmd; n++)
		{
			if (strncasecmp(commands[n].cmd, name, len) != 0) continue;
			if (list.size()) list += ", ";
			list += "/";
			list += commands[n].cmd;
		}
		cmd_error("Ambiguous command: `/%s' matches %s", name, list.c_str());
		reported = true;
	}

	return 0;
}

void parseCommand(const char* line, size_t size)
{
	if (!line) return;

	std::string input(line, size);

	/* The console hands us the line without its newline, but be tolerant. */
	while (input.size() && isspace((unsigned char) input[input.size()-1]))
		input.erase(input.size()-1);

	if (input.empty() || input[0] != '/') return;

	size_t split = input.find(' ');
	std::string name = (split == std::string::npos) ? input.substr(1) : input.substr(1, split - 1);
	std::string rest = (split == std::string::npos) ? std::string() : input.substr(split + 1);

	if (name.empty())
	{
		cmd_error("Empty command. Try `/help'");
		return;
	}

	bool reported = false;
	const struct CommandData* command = matchCommand(name.c_str(), reported);
	if (!command)
	{
		if (!reported)
			cmd_error("Unknown command: `/%s'. Try `/help'", name.c_str());
		return;
	}

	if (!command->handler)
	{
		cmd_error("`/%s' is not implemented yet", command->cmd);
		return;
	}

	std::vector<std::string> args;
	if (!splitArguments(command, rest.c_str(), args)) return;

	command->handler(args);
}
