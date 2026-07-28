/*
 * Copyright (C) 2001-2010 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_SOCKETREAPER_H
#define HAVE_QUICKDC_SOCKETREAPER_H

#include <memory>

namespace Samurai {
	namespace IO {
		namespace Net {
			class Socket;
		}
	}
}

namespace QuickDC {

/**
 * Hand over the last reference to a socket while one of its own event handlers
 * is running.
 *
 * The socket monitor dispatches from a pointer it stored when the socket was
 * registered and goes on using the socket after the handler returns - closing
 * the descriptor and deregistering it - so a handler that releases the last
 * reference leaves the rest of that dispatch reading freed memory. Parking the
 * socket here holds it until reapSockets() runs between two polls, where no
 * dispatch is in progress.
 *
 * The socket is dead either way: clear its event handler first, so nothing is
 * delivered to the object that just went away.
 */
void retireSocket(std::shared_ptr<Samurai::IO::Net::Socket> socket);

/**
 * Release every socket retireSocket() has collected. Only safe to call with no
 * socket event dispatch in progress; QuickDC::Core::run() does so after the
 * monitor returns.
 */
void reapSockets();

}

#endif // HAVE_QUICKDC_SOCKETREAPER_H
