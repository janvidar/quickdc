/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLIENTSESSION_H
#define HAVE_QUICKDC_CLIENTSESSION_H

namespace QuickDC {

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
		 * Transfer stopped, you need to check the status in the 
		 * transfer object.
		 */
		virtual void onTransferStopped() = 0;
		
};

}

#endif // HAVE_QUICKDC_CLIENTSESSION_H
