/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_REMOTE_H
#define HAVE_QUICKDC_CLI_REMOTE_H

#include "runner.h"
#include "script.h"
#include "session.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

/*
 * One script across several qdc processes. An agent runs cores and takes the
 * lines addressed to them from one controller; the controller reads the script,
 * runs the lines that look at events itself, over every agent's events, and
 * hands each other line to the agent of the core it names.
 *
 * Both ways a message is one line of JSON. To an agent, first {"key":"..."}, the
 * QDC_AGENT_KEY both sides have (empty when unset), then:
 *   {"run":"<line, definitions expanded>"}   answered by {"qdc":"done","outcome":"next|failed|quit"}
 *   {"start":true}                            the cores start; answered by done
 *   {"remember":"NAME","value":"..."}         a $NAME the controller learned
 *   {"stop":true}                             answered by {"qdc":"stopped"}, then the agent ends
 * From an agent, besides those: {"qdc":"hello","cores":[...]} first, every event
 * as qdc prints it, and {"qdc":"failed","why":...}, {"qdc":"note","text":...},
 * {"qdc":"remember","name":...,"value":...}, {"qdc":"forget","core":...}.
 */
namespace QuickDC::Cli {

/**
 * A socket listening on 'endpoint': a path for a Unix socket, HOST:PORT, or a
 * bare PORT on the loopback address. -1 with 'why' saying why it could not;
 * otherwise 'bound' is the endpoint as it can be connected to.
 */
int listenOn(const std::string& endpoint, std::string& bound, std::string& why);

/**
 * Take one controller on 'listener' and run what it sends against 'session',
 * whose cores are 'cores'; 'start' builds them. Returns once the controller
 * stops or goes away, with the cores stopped.
 */
int serveController(int listener, const std::vector<std::string>& cores, Runner& runner, Session& session,
                    const std::function<bool()>& start);

/**
 * Run 'statements', read with 'grammar', over the agents of 'agents' (core
 * name, endpoint), hearing their events into 'session'.
 */
int control(const std::vector<std::pair<std::string, std::string>>& agents, const Grammar& grammar,
            const std::vector<Statement>& statements, Session& session);

}

#endif // HAVE_QUICKDC_CLI_REMOTE_H
