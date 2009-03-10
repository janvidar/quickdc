/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef MINGW
#define SEARCH_DEBUG
#endif

#include "quickdc.h"
#include <stdio.h>
#include <string>
#include <string.h>
#include "sharemanager.h"
#include "sharedir.h"
#include "sharefile.h"
#include "config/preferences.h"
#include <samurai/io/file.h>
#include <samurai/io/compression.h>
#include <samurai/io/buffer.h>
#include "hash/hashmanager.h"
#include "network/adc/cid.h"

#ifdef SEARCH_DEBUG
#include <sys/time.h>
#include <time.h>
#endif

#define BUFSIZE 65536

#define DATADUMP

QuickDC::Share::Manager::Manager(QuickDC::Preferences* config_, QuickDC::Hash::Manager* mgr) : size(0), files(0), config(config_), hash(mgr), filelists(false) {

}

void QuickDC::Share::Manager::initialize() {
	QDBG("QuickDC::Share::Manager::initialize()");
	
	config->setGroup("Shares");
	int num = config->getNumber("Total shares", 0);

	QDBG("Shares found: %d", num);

	/**
	 * We store the directories shared in the following format:
	 * "fakename:/real/path/name"
	 */
	for (int i = 0; i < num; i++) {
		char key[16];
		sprintf(key, "Share %d", i);
		char* data = strdup(config->getString(key, ""));
		
		QDBG("Share %d='%s' (%s)", i, data, key);
		
		if (strlen(data) == 0) {
			config->remove(key);
		} else {
			char* split = strchr(data, ':');
			if (split && strlen(split) > 0) {
				split[0] = '\0';
				char* fakepath = data;
				char* realpath = &split[1];
				QuickDC::Share::Dir* dir = new QuickDC::Share::Dir(realpath, fakepath, this);
				this->addShare(dir);
			} else {
				config->remove(key);
			}
		}
		free(data);
	}
	createFileLists(true);
}

QuickDC::Share::Manager::~Manager() {
	/* delete all shares here, they will remove themselves */
	while (dirs.size()) {
		QuickDC::Share::Dir* dir = dirs.back();
		dirs.pop_back();
		delete dir;
	}
}

void QuickDC::Share::Manager::addSize(uint64_t sz) {
	size += sz;
	files++;
}

void QuickDC::Share::Manager::addShare(QuickDC::Share::Dir* dir) {
	dirs.push_back(dir);
}

void QuickDC::Share::Manager::removeShare(QuickDC::Share::Dir* dir) {
	dir->manager = 0;
}

Samurai::IO::File* QuickDC::Share::Manager::getFile(const char* pubname) const {
#ifdef DATADUMP
	QDBG("QuickDC::Manager::getFile(): '%s'", pubname);
#endif

	if (strcmp(pubname, "MyList.DcLst")  == 0 ||
		strcmp(pubname, "MyList.bz2")    == 0 ||
		strcmp(pubname, "files.xml")     == 0 ||
		strcmp(pubname, "files.xml.bz2") == 0 ) {
		
		if (!filelists)
		{
			QERR("Filelists not generated yet");
			return 0;
		}
		else
		{
			char* tmp = (char*) malloc(sizeof("~/.quickdc/") + strlen(pubname) + 1);
			tmp[0] = 0;
			strcat(tmp, "~/.quickdc/");
			strcat(tmp, pubname);
			Samurai::IO::File* file = new Samurai::IO::File(tmp);
			free(tmp);

#ifdef DATADUMP
			QDBG("xxx Looking up file: %s (%s)", file->getName().c_str(), pubname);
#endif
			if (!file->exists())
			{
				delete file;
				return 0;
			}
			return file;
		}
	} else if (strncasecmp("TTHL/", pubname, 5)) {
		return 0;
	} else {
		
		char* path = strdup(pubname);
		char* iter = path;
		char* npos = 0;
		size_t part = 0;
		
		npos = strchr(iter, '/');
		if (!npos) return 0; // Must have at least one / unless file is a file list
		npos[0] = 0;
		
		QuickDC::Share::Dir* root = 0;
		for (size_t i = 0; i < dirs.size(); i++) {
			if (dirs[i]->getBaseName() == iter) {
				part++;
				iter = &npos[1];
				npos = strchr(iter, '/');
				root = dirs[i];
				break;
			}
		}
		
		QuickDC::Share::Dir* dir = root;
		while (dir) {
			QDBG("*** lookup: str='%s'", iter);
			if (npos) {
				dir = dir->getDir(iter);
				iter = &npos[1];
				npos = strchr(iter, '/');
				part++;
			} else {
				QDBG("*** looking for file in dir='%s', file='%s'", dir->getPath().c_str(), iter);
				QuickDC::Share::File* shf = dir->getFile(iter);
				if (shf)
				{
					QDBG("Found shared file");
					return (Samurai::IO::File*) &shf->getFile();
				} else {
					QDBG("Did not find this shared file");
				}
				return 0;
			}
		}
	}
	
#ifdef DATADUMP
	QDBG("Not found");
#endif
	return 0;
	
}

Samurai::IO::File* QuickDC::Share::Manager::getFileByTTH(const char* tth) {
	Samurai::IO::File* file = 0;
	std::map<std::string, QuickDC::Share::File*>::iterator it = tthTable.find(std::string(tth));
	if (it != tthTable.end()) {
		std::pair<std::string, QuickDC::Share::File*> current = *it;
		file = (Samurai::IO::File*) &current.second->getFile();
	}
	return file;
}


#if defined(BZIP2) || defined(ZLIB)
static bool process(Samurai::IO::Codec* codec, Samurai::IO::Buffer* input, Samurai::IO::Buffer* output) {
	size_t len = input->size();
	char* buffer = (char*) malloc(len);
	char* inbuf = buffer;
	char outbuf[BUFSIZE];
	input->pop(inbuf, len);
	for (bool more = true; more; ) {
		size_t n = BUFSIZE;
		size_t in = len;
		more = codec->exec(inbuf, in, outbuf, n);
		inbuf += in;
		len -= in;
		output->append(outbuf, n);
		if (!in) more = false;
	}
	free(buffer);
	return true;
}
#endif

void QuickDC::Share::Manager::createFileLists(bool incomplete) {
	QDBG("QuickDC::Share::Manager::createFileLists");
	Samurai::IO::Buffer* xmlout = new Samurai::IO::Buffer();
	
	createFileList(xmlout, 0, incomplete);
	
	{
#ifdef DATADUMP
		QDBG("XML file list will be %i bytes long", (int) xmlout->size());
#endif
		Samurai::IO::File xmllist("~/.quickdc/files.xml");
		if (xmllist.open(Samurai::IO::File::Write | Samurai::IO::File::Truncate)) {
			char* out = new char[xmlout->size()];
			xmlout->pop(out, xmlout->size());
			int written = xmllist.write(out, xmlout->size());
			if (written == -1) QDBG("Error writing files.xml");
			xmllist.close();
			delete[] out;
		}
	}

#ifdef BZIP2
	{
		Samurai::IO::File dclist("~/.quickdc/files.xml.bz2");
		Samurai::IO::BZip2Compressor* filter = new Samurai::IO::BZip2Compressor();
		Samurai::IO::Buffer* writer = new Samurai::IO::Buffer();
		bool ok = process(filter, xmlout, writer);
		if (ok && dclist.open(Samurai::IO::File::Write | Samurai::IO::File::Truncate)) {
			int written = dclist.write(writer, writer->size());
			if (written == -1) QDBG("Error writing files.xml.bz2");
			dclist.close();
		}
		delete filter;
		delete writer;
	}
#endif // BZIP2
#ifdef DATADUMP
	QDBG("Lists written to disk");
#endif
	
	delete xmlout;
	
	filelists = true;
}

#define NEWLINE "\r\n"
#define INDENT '\x9'
#define SEPARATOR '|'

#define XML_NEWLINE "\r\n"
#define XML_INDENT "\x9"

namespace XML {
	extern void escape(Samurai::IO::Buffer* buffer, const char* str);
}

/**
 * FIXME: Should replace any unsafe XML characters
 *        and convert to utf-8.
 */
void QuickDC::Share::Manager::createFileList(Samurai::IO::Buffer* xmlout, QuickDC::Share::Dir* dir, bool incomplete) {
	if (!dir)
	{
		config->setGroup("User");
		xmlout->append("<?xml version=\"1.1\" encoding=\"utf-8\" standalone=\"yes\"?>" XML_NEWLINE);
		xmlout->append("<FileListing Version=\"1\" CID=\"");
		xmlout->append(config->getString("Client ID", ""));
		xmlout->append("\" Generator=\"" PRODUCT "/" VERSION " (" SYSTEM "/" CPU ")\" Base=\"/\">" XML_NEWLINE);
		
		for (size_t i = 0; i < dirs.size(); i++) {
			QuickDC::Share::Dir* dir = dirs[i];
			
			xmlout->append(XML_INDENT "<Directory Name=\"");
			XML::escape(xmlout, dir->getBaseName().c_str());
			xmlout->append("\"");
			if (incomplete) xmlout->append(" Incomplete=\"1\"");
			xmlout->append(">" XML_NEWLINE);
			createFileList(xmlout, dir, incomplete);
			xmlout->append(XML_INDENT "</Directory>" XML_NEWLINE);
		}
		xmlout->append("</FileListing>" XML_NEWLINE);
		
	} else {
		
		// Add files to lists
		for (size_t i = 0; i < dir->files.size(); i++) {
			QuickDC::Share::File* file = dir->files[i];
			
			for (int tab = 0; tab < file->getLevel()+1; tab++) xmlout->append(XML_INDENT);

			xmlout->append("<File Name=\"");
			XML::escape(xmlout, file->getBaseFileName().c_str());
			xmlout->append("\" Size=\"");
			xmlout->append(file->getSize());
			xmlout->append("\" TTH=\"");
			if (file->getTTH())	xmlout->append(file->getTTH());
			xmlout->append("\" />" XML_NEWLINE);
		}
		
		// Add sub-directories
		for (size_t i = 0; i < dir->subdirs.size(); i++) {
			QuickDC::Share::Dir* mdir = dir->subdirs[i];

			for (int tab = 0; tab < mdir->getLevel()+1; tab++) xmlout->append(XML_INDENT);

			xmlout->append("<Directory Name=\"");
			XML::escape(xmlout, mdir->getBaseName().c_str());
			xmlout->append("\"");
			if (incomplete) xmlout->append(" Incomplete=\"1\"");
			xmlout->append(">" XML_NEWLINE);
			
			createFileList(xmlout, mdir, incomplete);
			
			for (int tab = 0; tab < mdir->getLevel()+1; tab++) xmlout->append(XML_INDENT);
			xmlout->append("</Directory>" XML_NEWLINE);
		}
	}
}

void QuickDC::Share::Manager::addLookupFile(const char* name, const char* tth, QuickDC::Share::File* ptr) {
	searchTable.push_back( std::pair<const char*, QuickDC::Share::File*>(name, ptr) );
	tthTable.insert( std::pair<std::string, QuickDC::Share::File*>(std::string(tth), ptr) );
}

#ifdef WIN32
extern char* quickdc_strcasestr(char* haystack, char* needle);
#endif

void QuickDC::Share::Manager::search(SearchRequest* req) {
	if (!req) return;
	
#ifdef SEARCH_DEBUG
	struct timeval before;
	gettimeofday(&before, 0);
#endif
	
	int max = req->getMaxResults();
	uint64_t size = req->getSize();
	enum Share::SearchRequest::SizePolicy size_type = req->getSizePolicy();
	int hits = 0;
	
	if (req->isTTH()) {

		/* Search TTH */
		QSEARCH("tth search for: %s", req->tth);
		std::map<std::string, QuickDC::Share::File*>::iterator it = tthTable.find(std::string(req->tth));
		if (it != tthTable.end()) {
			std::pair<std::string, QuickDC::Share::File*> current = *it;
			hits++;
			SearchReply reply(req, current.second);
			QSEARCH("(tth hit): %s", current.second->getPublicName().c_str());
			if (req->event_handler)
				req->event_handler->EventSearchReply(&reply);
		}
		
	} else {
		if (!req->include.size()) return; /* Invalid search */
	
		for (size_t n = 0; n < searchTable.size(); n++) {
			std::pair<const char*, QuickDC::Share::File*> current = searchTable[n];
			const char* line = current.first;
	
			/* inclusive search */
			for (size_t i = 0; i < req->include.size() && line; i++) {
#ifdef WIN32
				line = quickdc_strcasestr((char*) line, (char*) req->include[i]);
#else
				line = strcasestr(line, req->include[i]);
#endif
			
			}
			if (!line) continue;
			
			/* exclude search */
			for (size_t i = 0; i < req->exclude.size() && line; i++) {
#ifdef WIN32
				line = quickdc_strcasestr((char*) line, (char*) req->exclude[i]);
#else
				line = strcasestr(line, req->exclude[i]);
#endif
			}
			if (!line) continue;
			
			/* match extension */
			if (req->extension.size()) {
				bool extensionOK = false;
				for (size_t i = 0; i < req->extension.size() && line; i++) {
					if (current.second->getFile().matchExtension(req->extension[i])) {
						extensionOK = true;
						break;
					}
				}
				if (!extensionOK) continue;
			}
			
			/* match size */
			if (current.second->match(size, size_type)) {
				current.second->hit();
				hits++;
				SearchReply reply(req, current.second);
				QSEARCH("(hit)   : %s", current.second->getPublicName().c_str());
				if (req->event_handler)
					req->event_handler->EventSearchReply(&reply);
			}
			
			
			// Return when reaching max results limit
			if (hits >= max) break;
		}
	}
	
#ifdef SEARCH_DEBUG
	struct timeval after;
	gettimeofday(&after, 0);
	long ms = (((after.tv_usec - before.tv_usec) / 1000) + ((after.tv_sec - before.tv_sec) * 1000));
	QSEARCH("Search '%p', returned %d hits in %d ms", req, hits, (int) ms);
#endif
}


