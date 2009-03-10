/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "share/dcfilelist.h"
#include <vector>
#include "xml/reader.h"
#include <samurai/io/file.h>
#include <samurai/io/buffer.h>

Share::RemoteFile::RemoteFile(const char* name_, uint64_t size_, const char* tth_, RemoteDir* parent_) {
	name = strdup(name_);
	size = size_;
	tth = strdup(tth_);
	parent = parent_;
	if (parent)
		parent->addSize(size);
}

Share::RemoteFile::~RemoteFile() {
	free(name);
	free(tth);
}

Share::RemoteDir::RemoteDir(const char* name_, Share::RemoteDir* parent_) {
	name = strdup(name_);
	parent = parent_;
	if (parent)
		parent->subdirs.push_back(this);
}

Share::RemoteDir::~RemoteDir() {
	while (subdirs.size()) {
		Share::RemoteDir* dir = subdirs.back();
		subdirs.pop_back();
		delete dir;
	}

	while (files.size()) {
		Share::RemoteFile* file = files.back();
		files.pop_back();
		delete file;
	}
	free(name);
}
		
void Share::RemoteDir::addSize(uint64_t size_) {
	size += size_;
	filecount++;
		
	if (parent)
		parent->addSize(size_);
}



Share::XMLFileListHandler::XMLFileListHandler(RemoteDir* root_, Samurai::IO::Buffer* buffer) {
	reader = new XML::SAX::XMLReader(buffer, this);
	root = root_;
}

Share::XMLFileListHandler::~XMLFileListHandler() {
	delete reader;
}

bool Share::XMLFileListHandler::parse() {
	problem = false;
	reader->parse();
	return !problem;
}

void Share::XMLFileListHandler::startDocument() {
}

void Share::XMLFileListHandler::endDocument() {
}

void Share::XMLFileListHandler::startElement(const char*, const char* localName, const char*, AttributeList list) {
	if (strcasecmp(localName, "FileListing") == 0) {
		if (currentDir) onProblem();
		currentDir = root;
	} else if (strcasecmp(localName, "Directory") == 0) {
		if (!currentDir) { onProblem(); return; }
		// Add Directory to the currentDir.
		
		const char* name = 0;
		for (size_t n = 0; n < list.size(); n++) {
			if (strcasecmp(list[n].first, "Name") == 0)
				name = list[n].second;
		}
		
		if (name) {
			RemoteDir* dir = new RemoteDir(name, currentDir);
			currentDir = dir;
		}
	} else if (strcasecmp(localName, "File") == 0) {
		if (!currentDir) { onProblem(); return; }
		const char* name = 0;
		const char* tth = 0;
		uint64_t size = 0;
		
		for (size_t n = 0; n < list.size(); n++) {
			if (strcasecmp(list[n].first, "Name") == 0)
				name = list[n].second;
			else if (strcasecmp(list[n].first, "TTH") == 0)
				tth = list[n].second;
			else if (strcasecmp(list[n].first, "Size") == 0)
				size = quickdc_atoull(list[n].second);
		}
		if (name && tth)
			new Share::RemoteFile(name, size, tth, currentDir);
	}
}

void Share::XMLFileListHandler::endElement(const char*, const char* localName, const char*) {
	if (strcasecmp(localName, "Directory") == 0) {
		if (!currentDir) { onProblem(); return; }
		currentDir = currentDir->getParent();
	}
}

void Share::XMLFileListHandler::characters(const char*, size_t, size_t)
{
}

void Share::XMLFileListHandler::ignorableWhitespace(const char*, size_t, size_t)
{
}

void Share::XMLFileListHandler::warning(const char* str) { QERR("XML WARNING: %s", str); }
void Share::XMLFileListHandler::error(const char* str)   { QERR("XML ERROR:   %s", str); }
void Share::XMLFileListHandler::fatalError(const char*)  { onProblem(); }

void Share::XMLFileListHandler::onProblem() { problem = true; }



Share::RemoteFileList::RemoteFileList(Samurai::IO::File* file_) {
	user = 0;
	file = file_;
	buffer = 0;
	root = new RemoteDir("", 0);
}
		
Share::RemoteFileList::~RemoteFileList() { }
	
bool Share::RemoteFileList::decode() {
	size_t size = (size_t) file->size();
	if (!file->open(Samurai::IO::File::Read)) {
		QERR("Unable to open file");
		return false;
	}

	uint8_t* temp_buffer = new uint8_t[size];
	file->read((char*) temp_buffer, size);
	buffer = new Samurai::IO::Buffer(size);
	buffer->append((char*) temp_buffer, size);
	delete[] temp_buffer;
	
	XMLFileListHandler* handler = new XMLFileListHandler(root, buffer);
	bool ret = handler->parse();
	
	file->close();
	
	return ret;
}




