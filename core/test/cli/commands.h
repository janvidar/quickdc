/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CLI_COMMANDS_H
#define HAVE_QUICKDC_CLI_COMMANDS_H

namespace QuickDC {
	class User;
}

struct CommandData {
	const char* cmd;
	size_t     length;
	size_t     params;
	const char* args; /* n = nick/handle,  s = string (spaced), i = number. ?s = optional string (can only be used once)*/
	const char* description;
	int hidden;
};
 

class Arg {
	public:
		virtual ~Arg() { }
		virtual bool isValid() = 0;
};

class ArgHandle : public Arg {
	public:
		ArgHandle(char* s);
		~ArgHandle();
		bool isValid();
		QuickDC::User* user;
};

class ArgString : public Arg {
	public:
		ArgString(char* s);
		~ArgString();
		bool isValid();
		char* string;
};

class ArgInteger : public Arg {
	public:
		ArgInteger(char* s);
		~ArgInteger();
		bool isValid();
		int number;
		bool valid;
};

typedef std::vector<Arg*> argumentList;

class Command {
	public:
		Command(const char* line, size_t size);
		~Command();
		
		bool isValid();
		
	protected:
		bool match();
		bool parse();

		char* line;
		char* cmd;
		bool valid;
		std::vector<Arg*> arguments;
};

class CommandHandler
{
	public:
		CommandHandler(const char* cmd);
		virtual ~CommandHandler();
		
		const char* getCommand() const;
		virtual void exec(argumentList arguments) = 0;
		
	protected:
		const char* cmd;
};

class CommandHandlerManager {
	public:
		CommandHandlerManager();
		virtual ~CommandHandlerManager();
		
		void add(CommandHandler* handler);
		
	private:
		std::vector<CommandHandler*> handlers;
};


extern void parseCommand(const char* cmd, size_t size);

#endif // HAVE_QUICKDC_CLI_COMMANDS_H
