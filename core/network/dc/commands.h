/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_NMDC_COMMANDS_H
#define HAVE_QUICKDC_NMDC_COMMANDS_H

#include "network/dc/dccommandparser.h"

namespace DC {

/* Client-to-hub commands */
class SendValidateNick : public Command
{
	public:
		SendValidateNick(const char* nick);
};

class SendVersion : public Command
{
	public:
		SendVersion();
};

class SendGetNickList : public Command
{
	public:
		SendGetNickList();
};

class SendSupports : public Command
{
	public:
		SendSupports();
};

class SendMyPass : public Command
{
	public:
		SendMyPass(const char* password);
};

class SendGetInfo : public Command
{
	public:
		SendGetInfo(const char* nick, const char* me);
};

class SendMyInfo : public Command
{
	public:
		SendMyInfo(const char* nick, const char* description, const char* email, uint64_t shared_size);
};

class SendTo : public Command
{
	public:
		SendTo(const char* from, const char* to, const char* message);
};

class SendMessage : public Command
{
	public:
		SendMessage(const char* from, const char* message);
};

class SendConnectToMe : public Command
{
	public:
		SendConnectToMe(const char* to, const char* address, uint16_t port);
};

class SendRevConnectToMe : public Command
{
	public:
		SendRevConnectToMe(const char* from, const char* to);
};

/* this is used both client-to-client and client-to-hub */
class SendKey : public Command
{
	public:
		SendKey(const char* key, size_t length);
};

/* send an empty command, which is used to check if the link is still up. */
class SendEmptyCommand : public Command
{
	public:
		SendEmptyCommand();
};

/* Hub admin commands */
class SendKick : public Command
{
	public:
		SendKick(const char* user);
};

class SendRedirect : public Command
{
	public:
		SendRedirect(const char* user, const char* address, const char* message);
};

/* Client-to-client commands */
class SendMyNick : public Command
{
	public:
		SendMyNick(const char* nick);
};

class SendLock : public Command
{
	public:
		SendLock(const char* lock, const char* pk);
};

class SendClientSupports : public Command
{
	public:
		SendClientSupports();
};

class SendDirection : public Command
{
	public:
		SendDirection(bool download, size_t priority);
};

class SendErrorFileNotAvailable : public Command
{
	public:
		SendErrorFileNotAvailable();
};

class SendFileLength : public Command
{
	public:
		SendFileLength(uint64_t size);
};

/* NOTE: This should be removed! -- legacy stuff */
class SendListLength : public Command
{
	public:
		SendListLength(uint64_t size);
};


}

#endif // HAVE_QUICKDC_NMDC_COMMANDS_H
