/*
 * Copyright (C) 2026 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_SOCKETS_H
#define HAVE_QUICKDC_CLI_SOCKETS_H

#include <fcntl.h>
#include <sys/socket.h>

namespace QuickDC::Cli {

/* Sockets closed on exec, so an agent's children do not hold them; macOS has no SOCK_CLOEXEC or accept4. */

#if !defined(SOCK_CLOEXEC)
inline int closingOnExec(int fd)
{
	if (fd != -1) fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}
#endif

inline int streamSocket(int family)
{
#if defined(SOCK_CLOEXEC)
	return ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
	return closingOnExec(::socket(family, SOCK_STREAM, 0));
#endif
}

inline int acceptClosing(int listener)
{
#if defined(SOCK_CLOEXEC)
	return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
#else
	return closingOnExec(::accept(listener, nullptr, nullptr));
#endif
}

}

#endif // HAVE_QUICKDC_CLI_SOCKETS_H
