/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "network/adc/parser.h"
#include <samurai/io/buffer.h>

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
	command.fourcc = ((buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | (buffer[3]));
	
	char* buf = strndup(buffer, buflen);

	// Always true!
	if (buf[buflen] == '\n') buf[buflen] = 0;

	if (buflen > 5) {
		char* last = &buf[5];
		char* offset = strchr(last, ' ');
		
		while (offset) {
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

	if (command.cached) free(command.cached);
	
	
	command.cached = (char*) malloc(strlen(buffer) + 2);
	command.cached[0] = 0;
	strcat(command.cached, buffer);
	strcat(command.cached, "\n");
	command.cachedSize = buflen+1;
	
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

ADC::Command::Command(Command* copy) : source_id(0), target_id(0), socket(0), cached(0), cachedSize(0), priority(Normal)
{
	fourcc = copy->fourcc;
	if (copy->source_id) source_id = strdup(copy->source_id);
	if (copy->target_id) target_id = strdup(copy->target_id);
	
	for (std::vector<char*>::iterator it = copy->features_include.begin(); it != copy->features_include.end(); it++)
		features_include.push_back((*it));
	
	for (std::vector<char*>::iterator it = copy->features_exclude.begin(); it != copy->features_exclude.end(); it++)
		features_exclude.push_back((*it));
	
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

ADC::Command::Command() : source_id(0), target_id(0), socket(0), cached(0), cachedSize(0), priority(Normal)
{

}

ADC::Command::Command(uint32_t fourcc_, const char* source_id_, const char* target_id_) : source_id(0), target_id(0), socket(0), cached(0), cachedSize(0), priority(Normal)
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
	free(cached);
	cached = 0;
	cachedSize = 0;
}

void ADC::Command::addArgument(const char* name, const char* arg)
{
	if (!arg) return;
	if (cached) resetCache();
	
	char* tmp = (char*) malloc(strlen(arg) + 3);
	memset(tmp, 0, strlen(arg) + 2);
	strncpy(tmp, name, 2);
	strcat(tmp, arg);
	arguments.push_back(tmp);
}

void ADC::Command::addArgument(const char* name, const std::string& arg)
{
	if (cached) resetCache();
	
	char* tmp = (char*) malloc(arg.size() + 3);
	memset(tmp, 0, arg.size() + 2);
	strncpy(tmp, name, 2);
	strcat(tmp, arg.c_str());
	arguments.push_back(tmp);
}


void ADC::Command::addArgument(const char* arg)
{
	if (!arg) return;
	if (cached) resetCache();
	
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

bool ADC::Command::haveArgument(const char* name, size_t len, size_t after)
{
	for (size_t n = after; n < arguments.size(); n++)
	{
		if (strncmp(name, arguments[n], len) == 0)
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
		if (strncmp(name, arguments[n], len) == 0)
			return &arguments[n][len];
	}
	return 0;
}

bool ADC::Command::removeArgument(const char* name, size_t len, size_t after)
{
	if (cached) resetCache();

	bool found = false;
	std::vector<char*> copy;
	for (size_t n = 0; n < arguments.size(); n++)
	{
		if (n >= after)
		{
			if (strncmp(name, arguments[n], len) == 0)
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
	if (cachedSize)
		return cachedSize;
	
	write(0);
	return cachedSize;
}

char* ADC::Command::getRawCommand()
{
	if (cachedSize)
		return cached;
	
	write(0);
	return cached;
}


void ADC::Command::write(Samurai::IO::Buffer* stream_)
{
	Samurai::IO::Buffer* stream = stream_;
	if (!stream_) stream = new Samurai::IO::Buffer();
	
	if (cached)
	{
		stream->append(cached);
		return;
	}
	
	size_t start = stream->size();
	switch ((char) ((fourcc & FOURCC(0xff,0,0,0)) >> 24))
	{
		case 'C':
		case 'I':
		case 'H':
			stream->appendBinary(fourcc, Samurai::IO::Buffer::BigEndian);
			break;
	
		case 'B':
		case 'U':
#ifdef NAT_TRAVERSAL_SUPPORT
		case 'X':
#endif
			if (!source_id) return;
			stream->appendBinary(fourcc, Samurai::IO::Buffer::BigEndian);
			stream->append(' ');
			stream->append(source_id);
			break;
		
		case 'E':
		case 'D':
			if (!source_id || !target_id) return;
			stream->appendBinary(fourcc, Samurai::IO::Buffer::BigEndian);
			stream->append(' ');
			stream->append(source_id);
			stream->append(' ');
			stream->append(target_id);
			break;
		
		case 'F':
			if (!source_id || !features_include.size() || !features_exclude.size()) return;
			stream->appendBinary(fourcc, Samurai::IO::Buffer::BigEndian);
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
	cached = stream->memdup(start, stream->size());
	cachedSize = (stream->size() - start);
	
	if (!stream_)
		delete stream;
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




