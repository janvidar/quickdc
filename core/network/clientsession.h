/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLIENTSESSION_H
#define HAVE_QUICKDC_CLIENTSESSION_H

namespace QuickDC {

class Transfer;

class ClientSession
{
	public:
		
		ClientSession() { }
		virtual ~ClientSession() { }
		
		/**
		 * Launch session as server.
		 * Typically the client speaks first.
		 */
		virtual void onStartServer() = 0;
		
		/**
		 * Launch session as client.
		 */
		virtual void onStartClient() = 0;
		
		/**
		 * A transfer stopped; check the status on it.
		 *
		 * @param which the transfer that stopped, which is not necessarily the one
		 *        this session is holding - see PrivateTransferListener, which
		 *        declares the same thing for the same reason.
		 */
		virtual void onTransferStopped(Transfer* which) = 0;
		
};

}

#endif // HAVE_QUICKDC_CLIENTSESSION_H
