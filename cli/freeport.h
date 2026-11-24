/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_FREEPORT_H
#define HAVE_QUICKDC_CLI_FREEPORT_H

namespace QuickDC::Cli {

/**
 * A port that both TCP and UDP could bind on every address just now, or -1
 * with errno set. No other qdc is handed it while this one runs; anything else
 * may still take it, so a script should claim it promptly.
 */
int freePort();

}

#endif // HAVE_QUICKDC_CLI_FREEPORT_H
