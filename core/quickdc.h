/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_TYPES_H
#define HAVE_QUICKDC_TYPES_H

#include <samurai/samurai.h>
#include <samurai/debug/dbg.h>
#include <samurai/timestamp.h>
#include <samurai/messagehandler.h>
#include <samurai/stdc.h>

#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

namespace QuickDC
{
enum MessageID {
	/* Control */
	MsgCoreCPUOverload        = 1001,   /**<< Don't accept new searches, don't compress transfers, etc. */
	MsgCoreCPUOK              = 1002,   /**<< Clear overload flag */
	
	/* Socket monitor - internals */
	MsgSocketMonitorAdd       = 2000,   /**<< Add a socket to the socket monitor */
	MsgSocketMonitorModify    = 2001,   /**<< Modify a socket in the socket monitor */
	MsgSocketMonitorRemove    = 2002,   /**<< Remove a socket from the socket monitor */
	MsgSocketMonitorDelete    = 2003,   /**<< Remove a socket from the socket monitor, and then delete it. */
	
	/* Config */
	MsgConfigUpdated          = 3000,   /**<< Make sure we send updated info whenever possible */
	MsgConfigNetworkUpdated   = 3001,   /**<< Networking information has changed, need to reapply configuration */
	
	/* Share related */
	MsgShareChanged           = 4001,   /**<< Share size have changed, all hubs should be notified */
	MsgShareRebuilding        = 4002,   /**<< Share database is rebuilding */
	MsgShareRebuilt           = 4003,   /**<< Share database is rebuilt */
	
	/* Hash related */
	MsgHashJobStarted         = 5001,   /**<< Hash job started */
	MsgHashJobStopped         = 5002,   /**<< Hash job stopped */
	
	/* Connection related */
	MsgConnectionAccepted     = 6000,   /**<< A connection has been accepted */
	MsgConnectionDropped      = 6001,   /**<< A connection has been dropped */
	
	/* Transfer related */
	MsgTransferStarted        = 7000,   /**<< A transfer job has started */
	MsgTransferStopped        = 7001,   /**<< A transfer job has stopped - and should be deleted */
	
	/* Local hub related */
	MsgHubUserConnected       = 8000,   /**<< A user connected to our local hub */
	MsgHubUserDisconnected    = 8001,   /**<< A user disconnected from our local hub */
	MsgHubUserLoggedIn        = 8002,   /**<< A user logged in to our local hub */
	MsgHubUserRemoveUser      = 8003,   /**<< A user should be removed from the local hub. */
	MsgHubUserAppendUser      = 8004,   /**<< A user should be added to the local hub. */
	MsgHubUserDelayedQuit     = 8005,   /**<< A user quit (usually error), post a message and process it later */
	
	/* Hub client related */
	MsgHubCountChanged        = 9000,   /**<< Our hub count data has changed - Notify hubs */
	MsgHubStopUserTransfers   = 9001,   /**<< A hub says stop transfers to a given user */
	
	MsgLAST
};
}

#endif // HAVE_QUICKDC_TYPES_H
