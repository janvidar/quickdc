/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/adc/parser.h"
#include <samurai/io/buffer.h>
#include <memory>
#include <string_view>

static bool isAlphaNum(char c) {
	if (c < 48 || c > 90) return false;
	if (c > 57 && c < 65) return false;
	return true;
}

/**
 * Verify feature cast string
 */
static bool verifyFeatures(const char* str) { 
	if (!str) return false;
	if (strlen(str) % 5 != 0) return false;
	
	for (size_t n = 0; n < strlen(str); n++) {
		if (n % 5 == 0) {
			if (!(str[n] == '-' || str[n] == '+')) return false;
		} else {
			if (!isAlphaNum(str[n])) return false;
		}
	}
	return true;
}

bool ADC::Parser::parse(const char* buffer, size_t buflen, ADC::Command& command) {
	if (buflen < 4 || !buffer) return false;

	/*
	 * buflen is the authoritative length, and every offset below assumes buf is
	 * exactly that long. A NUL inside the line breaks that: strndup would stop
	 * at it and allocate less, leaving &buf[5] and the terminator check reading
	 * past the end. ADC has no use for one, so refuse the line instead.
	 */
	if (memchr(buffer, '\0', buflen)) return false;

	/*
	 * The fifth byte is the separator. Without checking it "BINFX AAAA" parses
	 * as a BINF whose source ID is the empty string, because the split starts at
	 * the sixth byte regardless of what the fifth one was.
	 */
	if (buflen > 4 && buffer[4] != ' ') return false;

	/* Unsigned, or a byte with the top bit set sign-extends into the fourcc. */
	command.fourcc = (((uint32_t) (unsigned char) buffer[0] << 24)
	                | ((uint32_t) (unsigned char) buffer[1] << 16)
	                | ((uint32_t) (unsigned char) buffer[2] << 8)
	                |  (uint32_t) (unsigned char) buffer[3]);

	char* buf = (char*) malloc(buflen + 1);
	if (!buf) return false;
	memcpy(buf, buffer, buflen);
	buf[buflen] = 0;

	if (buflen > 5) {
		char* last = &buf[5];
		char* offset = strchr(last, ' ');
		
		while (offset) {
			if (command.arguments.size() >= ADC::MAX_COMMAND_ARGUMENTS) {
				free(buf);
				return false;
			}
			size_t len = &offset[0]-&last[0];
			char* argument = strndup(&last[0], len);
			command.arguments.push_back(unescape(&argument, len));
			if (strlen(offset)) {
				last = &offset[1];
				offset = strchr(&offset[1], ' ');
			} else {
				offset = 0;
			}
		}

		if (last) {
			if (command.arguments.size() >= ADC::MAX_COMMAND_ARGUMENTS) {
				free(buf);
				return false;
			}
			char* argument = strdup(last);
			command.arguments.push_back(unescape(&argument, strlen(argument)));
		}
	}
	
	free(buf);
	
	switch (buffer[0]) {
		case 'C':
		case 'I':
		case 'H':
			break;
		
		case 'B':
		case 'U':
			if (command.arguments.size() < 1) return false;
			command.source_id = command.arguments.front(); command.arguments.erase(command.arguments.begin());
			break;
		
		case 'E':
		case 'D':
			if (command.arguments.size() < 2) return false;
			command.source_id = command.arguments.front(); command.arguments.erase(command.arguments.begin());
			command.target_id = command.arguments.front(); command.arguments.erase(command.arguments.begin());
			break;
		
		case 'F':
		{
			if (command.arguments.size() < 2) return false;
			command.source_id = command.arguments.front(); command.arguments.erase(command.arguments.begin());
			char* features = command.arguments.front(); command.arguments.erase(command.arguments.begin());
			if (!verifyFeatures(features)) {
				free(features);
				return false;
			} else {
				char* it = features;
				while (strlen(it)) {
					bool include = (it[0] == '+');
					char* featureName = strndup(&it[1], 4);
					if (include)
						command.features_include.push_back(featureName);
					else
						command.features_exclude.push_back(featureName);
					it = &it[5];
				}
			}
			free(features);
			break;
		}
		default:
			return false;
	}

	/*
	 * Sized from buflen, not from strlen(buffer). The two disagreed whenever the
	 * caller's buffer was not terminated exactly where buflen said - a datagram
	 * without a trailing newline reads past the end, and a line carrying a NUL
	 * was shorter than the size claimed, which the hub went on to hand to
	 * write() and broadcast whatever followed it on the heap.
	 */
	command.cached.assign(buffer, buflen);
	command.cached += '\n';

	return true;
}

char* ADC::Parser::unescape(char** str_, size_t max)
{
	char* str = *str_;
	size_t newsize = 0;
	bool escaped = false;
	for (size_t i = 0; i < max; i++) {
		if (escaped) {
			switch (str[i]) {
				case '\\': str[newsize] = '\\'; break;
				case 's' : str[newsize] = ' ';  break;
				case 'n' : str[newsize] = '\n'; break;
				default:
					str[newsize] = str[i];
			}
			escaped = false;
			newsize++;
		} else {
			if (str[i] == '\\') escaped = true;
			else str[newsize++] = str[i];
		}
	}
	str[newsize] = '\0';
	return str;
}


size_t ADC::Parser::escapeLength(const char* str)
{
	size_t add = 0;
	size_t n = 0;
	for (; str[n]; n++)
		if (str[n] == ' ' || str[n] == '\n' || str[n] == '\\') add++;
	return n + add;
}

void ADC::Parser::escape(const char* old, size_t oldsize, char* str, size_t max)
{
	size_t n = 0;
	for (size_t i = 0; i < oldsize && n < max; i++) {
		switch (old[i]) {
			case '\\': /* fall through */
				str[n++] = '\\';
				str[n++] = '\\';
				break;
			case '\n':
				str[n++] = '\\';
				str[n++] = 'n';
				break;
			case ' ':
				str[n++] = '\\';
				str[n++] = 's';
				break;
			default:
				str[n++] = old[i];
				break;
		}
	}
	str[n] = '\0';
}


/* **************************************************************************************************** */
/* **************************************************************************************************** */
/* **************************************************************************************************** */

ADC::Command::Command(Command* copy) : source_id(0), target_id(0), socket(0), priority(Normal)
{
	fourcc = copy->fourcc;
	if (copy->source_id) source_id = strdup(copy->source_id);
	if (copy->target_id) target_id = strdup(copy->target_id);
	
	/* Duplicated, like everything else here: both destructors free their own,
	   so sharing the pointers frees each of these twice. */
	for (std::vector<char*>::iterator it = copy->features_include.begin(); it != copy->features_include.end(); it++)
		features_include.push_back(strdup(*it));

	for (std::vector<char*>::iterator it = copy->features_exclude.begin(); it != copy->features_exclude.end(); it++)
		features_exclude.push_back(strdup(*it));
	
	timestamp = copy->timestamp;
	priority = copy->priority;
	
	socket = copy->socket;
	for (size_t n = 0; copy->getArgument(n); n++)
	{
		char* arg = copy->getArgument(n);
		if (arg)
			arguments.push_back(strdup(arg));
	}
}

ADC::Command::Command() : source_id(0), target_id(0), socket(0), priority(Normal)
{

}

ADC::Command::Command(uint32_t fourcc_, const char* source_id_, const char* target_id_) : source_id(0), target_id(0), socket(0), priority(Normal)
{
	fourcc = fourcc_;
	if (source_id_) source_id = strdup(source_id_);
	if (target_id_) target_id = strdup(target_id_);
}

ADC::Command::~Command()
{
	while (arguments.size())
	{
		char* arg = arguments.back();
		arguments.pop_back();
		free(arg);
	}
	
	while (features_include.size())
	{
		char* feature = features_include.back();
		features_include.pop_back();
		free(feature);
	}
	
	while (features_exclude.size())
	{
		char* feature = features_exclude.back();
		features_exclude.pop_back();
		free(feature);
	}
	
	free(source_id);
	free(target_id);
	resetCache();
}


void ADC::Command::resetCache()
{
	cached.clear();
}

void ADC::Command::addArgument(const char* name, const char* arg)
{
	if (!arg) return;
	if (!cached.empty()) resetCache();
	
	const size_t length = strlen(arg);

	/*
	 * Two characters of name, the argument, and a terminator.
	 *
	 * The whole allocation is cleared, and the argument copied rather than
	 * appended. Zeroing one byte less than was allocated left the last byte as it
	 * came from the allocator, and strcat() then looked for the terminator that
	 * byte was supposed to be: with an empty argument - an empty description or
	 * e-mail field in a CINF is enough - it found whatever was there instead and
	 * wrote past the end of the buffer.
	 */
	char* tmp = (char*) malloc(length + 3);
	memset(tmp, 0, length + 3);
	strncpy(tmp, name, 2);
	memcpy(&tmp[2], arg, length);
	arguments.push_back(tmp);
}

void ADC::Command::addArgument(const char* name, const std::string& arg)
{
	if (!cached.empty()) resetCache();
	
	/* As above, and for the same reasons. */
	char* tmp = (char*) malloc(arg.size() + 3);
	memset(tmp, 0, arg.size() + 3);
	strncpy(tmp, name, 2);
	memcpy(&tmp[2], arg.data(), arg.size());
	arguments.push_back(tmp);
}


void ADC::Command::addArgument(const char* arg)
{
	if (!arg) return;
	if (!cached.empty()) resetCache();
	
	arguments.push_back(strdup(arg));
}

char* ADC::Command::getArgument(size_t offset)
{
	if (offset < arguments.size()) return arguments[offset];
	return 0;
}

bool ADC::Command::haveArgument(size_t offset)
{
	return (offset < arguments.size());
}

/*
 * 'len' is how much of the front of the argument the name occupies, not the
 * length of either string, so both sides are compared up to that much and no
 * further. Either one may be shorter than it, and then it only matches a side
 * that ends in the same place.
 */
static bool argumentHasName(const char* argument, const char* name, size_t len)
{
	const std::string_view arg(argument ? argument : "");
	const std::string_view key(name ? name : "");
	return arg.substr(0, len) == key.substr(0, len);
}

bool ADC::Command::haveArgument(const char* name, size_t len, size_t after)
{
	for (size_t n = after; n < arguments.size(); n++)
	{
		if (argumentHasName(arguments[n], name, len))
			return true;
	}
	return false;
}

size_t ADC::Command::countArguments()
{
	return arguments.size();
}

char* ADC::Command::getArgument(const char* name, size_t len, size_t after)
{
	for (size_t n = after; n < arguments.size(); n++) {
		if (argumentHasName(arguments[n], name, len))
			return &arguments[n][len];
	}
	return 0;
}

bool ADC::Command::removeArgument(const char* name, size_t len, size_t after)
{
	if (!cached.empty()) resetCache();

	bool found = false;
	std::vector<char*> copy;
	for (size_t n = 0; n < arguments.size(); n++)
	{
		if (n >= after)
		{
			if (argumentHasName(arguments[n], name, len))
			{
				free(arguments[n]);
				found = true;
			}
			else
			{
				copy.push_back(arguments[n]);
			}
		}
		else
		{
			copy.push_back(arguments[n]);
		}
	}
	arguments.clear();
	for (size_t n = 0; n < copy.size(); n++)
	{
		arguments.push_back(copy[n]);
	}
	return found;
}

size_t ADC::Command::getSize()
{
	if (!cached.empty())
		return cached.size();

	write(0);
	return cached.size();
}

const char* ADC::Command::getRawCommand()
{
	if (!cached.empty())
		return cached.c_str();

	write(0);
	return cached.c_str();
}


void ADC::Command::write(Samurai::IO::Buffer* stream_)
{
	/* Owned by this call when the caller supplied no buffer, so that it also
	   survives the returns below that abandon an incomplete command. */
	std::unique_ptr<Samurai::IO::Buffer> owned;
	if (!stream_) owned = std::make_unique<Samurai::IO::Buffer>();

	Samurai::IO::Buffer* stream = stream_ ? stream_ : owned.get();

	if (!cached.empty())
	{
		stream->append(cached.c_str(), cached.size());
		return;
	}

	size_t start = stream->size();
	switch ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24))
	{
		case 'C':
		case 'I':
		case 'H':
			stream->appendBinary(fourcc, Samurai::IO::Buffer::BinaryMode::BigEndian);
			break;
	
		case 'B':
		case 'U':
#ifdef NAT_TRAVERSAL_SUPPORT
		case 'X':
#endif
			if (!source_id) return;
			stream->appendBinary(fourcc, Samurai::IO::Buffer::BinaryMode::BigEndian);
			stream->append(' ');
			stream->append(source_id);
			break;
		
		case 'E':
		case 'D':
			if (!source_id || !target_id) return;
			stream->appendBinary(fourcc, Samurai::IO::Buffer::BinaryMode::BigEndian);
			stream->append(' ');
			stream->append(source_id);
			stream->append(' ');
			stream->append(target_id);
			break;
		
		case 'F':
			/* One list or the other is enough; a search that excludes nothing
			   is the ordinary case. */
			if (!source_id || (features_include.empty() && features_exclude.empty())) return;
			stream->appendBinary(fourcc, Samurai::IO::Buffer::BinaryMode::BigEndian);
			stream->append(' ');
			stream->append(source_id);
			stream->append(' ');
			for (std::vector<char*>::iterator it = features_include.begin(); it != features_include.end(); it++)
			{
				stream->append('+');
				stream->append((*it));
			}
			for (std::vector<char*>::iterator it = features_exclude.begin(); it != features_exclude.end(); it++)
			{
				stream->append('-');
				stream->append((*it));
			}
			
			break;
		default:
			break;
	}

	for (size_t n = 0; n < arguments.size(); n++)
	{
		char* arg = arguments[n];
		size_t nsize = ADC::Parser::escapeLength(arg);
		char* tmp = new char[nsize+1];
		ADC::Parser::escape(arg, strlen(arg), tmp, nsize);
		stream->append(' ');
		stream->append(tmp);
		delete[] tmp;
	}
	stream->append('\n');
	cached = stream->copyRange(start, stream->size());
}

bool ADC::Command::isCommandBroadcast()
{
	return ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24)) == 'B';
}

bool ADC::Command::isCommandClient()
{
	return ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24)) == 'C';
}

bool ADC::Command::isCommandDirect()
{
	return ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24)) == 'D';
}

bool ADC::Command::isCommandEcho()
{
	return ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24)) == 'E';
}

bool ADC::Command::isCommandFeature()
{
	return ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24)) == 'F';
}

bool ADC::Command::isCommandHub()
{
	return ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24)) == 'H';
}

bool ADC::Command::isCommandHubInfo()
{
	return ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24)) == 'I';
}

bool ADC::Command::isCommandUdp()
{
	return ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24)) == 'U';
}




