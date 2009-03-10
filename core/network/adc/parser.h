/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCPARSER_H
#define HAVE_QUICKDC_ADCPARSER_H

#include "quickdc.h"
#include <samurai/timestamp.h>
#include <vector>

#define FOURCC(a, b, c, d) ((uint32_t) ( (uint32_t)a << 24 | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d))

#include "network/commandproxy.h"
#include "network/adc/adccommon.h"


namespace Samurai {
	namespace IO {
		class Buffer;
		namespace Net {
			class Socket;
		}
	}
}

namespace ADC {

class Parser;
class CommandProxy;
class QueuedCommand;
class Hub;

/**
 * This represents an ADC command.
 */
class Command : public QuickDC::CommandBase
{
	public:
		enum Priority {
			Low,		/**<< Messages of low priority might be discarded for any reason. */
			Normal,		/**<< Messages of normal priority should not be discarded unless there is a good reason for it (eg. resources) */
			High		/**<< Messages of high priority should never be discarded */
		};
	
		Command();
		
		/**
		 * Perform a deep copy of the command.
		 */
		Command(Command* copy);
		
		/**
		 * Create and initialize a command.
		 */
		Command(uint32_t fourcc, const char* source_id = 0, const char* target_id = 0);
		
		virtual ~Command();
		
		/**
		 * @return the command fourcc.
		 */
		uint32_t getFourcc() const { return fourcc; }
		
		/**
		 * Add a named argument to the command.
		 * @param name a 2 character name prefix. (no checks are done to verify this)
		 * @param arg a variable length argument.
		 */
		void addArgument(const char* name, const char* arg);
		
		/**
		 * Add a named argument to the command.
		 * @param name a 2 character name prefix. (no checks are done to verify this)
		 * @param arg a variable length argument.
		 */
		void addArgument(const char* name, const std::string& arg);
		
		/**
		 * Add a argument to the command.
		 */
		void addArgument(const char* arg);
		
		/**
		 * @return true if an arguement at the given offset exists.
		 */
		bool haveArgument(size_t offset);
		
		/**
		 * @return true if a named argument exists after the given offset.
		 */
		bool haveArgument(const char* name, size_t len, size_t after = 0);
		
		/**
		 * @return true if a named argument exists after the given offset and is removed successfully.
		 */
		bool removeArgument(const char* name, size_t len, size_t after = 0);
		
		/**
		 * @return the number of arguments to the command
		 */
		size_t countArguments();
		
		/**
		 * @return the argument at the given offset or 0 if not found.
		 */
		char* getArgument(size_t offset);
		
		/**
		 * @return the given named argument if found after the given offset, or 0 if not found.
		 */
		char* getArgument(const char* name, size_t len, size_t after = 0);
		
		/**
		 * Retrieve the Source SID of the command (who the command was sent from)
		 * @return the command's source SID.
		 */
		char* getSourceID() { return source_id; }
		
		/**
		 * Retrieve the Source SID of the command (who the command was sent to)
		 * @return the command's target SID
		 */
		char* getTargetID() { return target_id; }
		
		/**
		 * FIXME: Does this thing really belong here?
		 * @return the socket associated with the command
		 */
		Samurai::IO::Net::Socket* getSocket() { return socket; }
		
		/**
		 * FIXME: Does this really belong here?
		 * Set a socket associated with the command.
		 */
		void setSocket(Samurai::IO::Net::Socket* socket_) { socket = socket_; }

		/**
		 * @return true if the command is a broadcast command.
		 */
		bool isCommandBroadcast();
		
		/**
		 * @return true if the command is a client-client command.
		 */
		bool isCommandClient();
		
		/**
		 * @return true if the command is supposed to be echoed back from the hub.
		 */
		bool isCommandEcho();
		
		/**
		 * @return true if the command is directed to one target only
		 */
		bool isCommandDirect();
		
		/**
		 * @return true if the command is a feature broadcast command
		 */
		bool isCommandFeature();
		
		/**
		 * @return true if the command is intended for a hub only
		 */
		bool isCommandHub();
		
		/**
		 * @return true if the command is from a hub to a client only.
		 */
		bool isCommandHubInfo();
		
		/**
		 * @return true if the command is a UDP command.
		 */
		bool isCommandUdp();

		/**
		 * @return the priority of this ADC command (mostly interresting for the hub).
		 */
		enum Priority getPriority() const { return priority; }
		
		/**
		 * Set the priority of this command.
		 */
		void setPriority(enum Priority prio) { priority = prio; }

		char* getRawCommand();
		
		size_t getSize();
		
		/**
		 * Writes the command to the given buffer.
		 */
		void write(Samurai::IO::Buffer* stream);
		
		/**
		 * Writes the command to the given buffer.
		 */
		void resetCache();


	protected:
		uint32_t fourcc;
		char* source_id;
		char* target_id;
		std::vector<char*> features_include;
		std::vector<char*> features_exclude;
		std::vector<char*> arguments;
		Samurai::IO::Net::Socket* socket;
		char* cached;
		size_t cachedSize;
		enum Priority priority;
		Samurai::TimeStamp timestamp;
		
	friend class ADC::Parser;
	friend class ADC::Hub;
	friend class ADC::CommandProxy;
	friend class ADC::QueuedCommand;
};

class Parser {
	public:
		
		/**
		* This will parse, split and unescape the ADC commands from the
		* given buffer and buffer length.
		* 
		* @param command will be completed by this method.
		* @return true if the command is a valid ADC command, otherwise false.
		*/
		static bool parse(const char* buffer, size_t buflen, Command& command);
		
		/**
		* This will unescape an ADC encoded string into something
		* readable. This must operate on pointers.
		*/
		static char* unescape(char** str_, size_t max);
		
		/**
		* This will escape readable text into ADC encoded data.
		*/
		/* static char* escape(char** str_, size_t max); */
		static void escape(const char* old, size_t oldsize, char* str, size_t max);

		static size_t escapeLength(const char* str);
};

}

#endif // HAVE_QUICKDC_ADCPARSER_H
