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
	if (parent) {
		parent->files.push_back(this);
		parent->addSize(size);
	}
}

Share::RemoteFile::~RemoteFile() {
	free(name);
	free(tth);
}

Share::RemoteDir::RemoteDir(const char* name_, Share::RemoteDir* parent_)
	: parent(parent_)
	, name(strdup(name_))
	, size(0)
	, filecount(0)
{
	if (parent)
		parent->subdirs.push_back(this);
}

/*
 * The shape of this tree came out of a remote peer's file list, so it is as
 * deep as that peer cared to make it: deleting a subdirectory by letting its
 * own destructor run would be one stack frame per level. Every descendant is
 * collected first, and each is emptied before it is deleted, so no destructor
 * below this one has anything left to descend into.
 */
Share::RemoteDir::~RemoteDir() {
	std::vector<Share::RemoteDir*> stack;
	stack.swap(subdirs);

	while (stack.size()) {
		Share::RemoteDir* dir = stack.back();
		stack.pop_back();

		for (size_t n = 0; n < dir->subdirs.size(); n++)
			stack.push_back(dir->subdirs[n]);
		dir->subdirs.clear();

		delete dir;
	}

	while (files.size()) {
		Share::RemoteFile* file = files.back();
		files.pop_back();
		delete file;
	}
	free(name);
}

/* Up the chain by walking it, for the same reason: this is called once per file
   the list holds, and the chain is as long as the list says. */
void Share::RemoteDir::addSize(uint64_t size_) {
	for (Share::RemoteDir* dir = this; dir; dir = dir->parent) {
		dir->size += size_;
		dir->filecount++;
	}
}



/*
 * currentDir doubles as the "are we inside a <FileListing>" flag, and every
 * element handler below tests it before walking through it. Left indeterminate
 * it is a pointer the file list's own bytes may well have supplied, so a list
 * that opens with anything other than <FileListing> writes through it.
 */
Share::XMLFileListHandler::XMLFileListHandler(RemoteDir* root_, Samurai::IO::Buffer* buffer)
	: reader(new XML::SAX::XMLReader(buffer, this))
	, root(root_)
	, currentDir(0)
	, skipped(0)
	, problem(false)
{
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

/*
 * How deeply a file list may nest. A list arrives bzip2 compressed and is
 * whatever the peer chose to send, so a few kilobytes of it expands to as many
 * levels as they like - and every level is a directory this holds on to and
 * later walks. Deeper than this is not a collection, it is the shape being used
 * as the payload.
 */
#define MAX_FILELIST_DEPTH 128

static size_t depthOf(const Share::RemoteDir* dir)
{
	size_t depth = 0;
	for (; dir; dir = dir->getParent())
		depth++;
	return depth;
}

void Share::XMLFileListHandler::startElement(const char*, const char* localName, const char*, const AttributeList& list) {
	const bool directory = (strcasecmp(localName, "Directory") == 0);

	/* Inside a subtree refused for its depth. Nothing in it is built, and the
	   directories in it are counted so that their end tags close them rather
	   than the one the list was cut off at. */
	if (skipped) {
		if (directory) skipped++;
		return;
	}

	if (strcasecmp(localName, "FileListing") == 0) {
		if (currentDir) onProblem();
		currentDir = root;
	} else if (directory) {
		if (!currentDir) { onProblem(); return; }
		// Add Directory to the currentDir.

		const char* name = 0;
		for (size_t n = 0; n < list.size(); n++) {
			if (strcasecmp(list[n].first.c_str(), "Name") == 0)
				name = list[n].second.c_str();
		}

		if (name && *name) {
			if (depthOf(currentDir) >= MAX_FILELIST_DEPTH) {
				QERR("File list nests deeper than %d directories; refusing it", MAX_FILELIST_DEPTH);
				onProblem();
				skipped = 1;
				return;
			}

			RemoteDir* dir = new RemoteDir(name, currentDir);
			currentDir = dir;
		}
	} else if (strcasecmp(localName, "File") == 0) {
		if (!currentDir) { onProblem(); return; }
		const char* name = 0;
		const char* tth = 0;
		uint64_t size = 0;
		
		for (size_t n = 0; n < list.size(); n++) {
			if (strcasecmp(list[n].first.c_str(), "Name") == 0)
				name = list[n].second.c_str();
			else if (strcasecmp(list[n].first.c_str(), "TTH") == 0)
				tth = list[n].second.c_str();
			else if (strcasecmp(list[n].first.c_str(), "Size") == 0)
				size = Samurai::Util::Convert::to_uint64(list[n].second.c_str());
		}
		if (name && *name && tth && *tth)
			new Share::RemoteFile(name, size, tth, currentDir);
	}
}

void Share::XMLFileListHandler::endElement(const char*, const char* localName, const char*) {
	if (strcasecmp(localName, "Directory") == 0) {
		if (skipped) { skipped--; return; }
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
		
Share::RemoteFileList::~RemoteFileList() {
	delete root;
	delete buffer;
}
	
bool Share::RemoteFileList::decode() {
	size_t size = (size_t) file->size();
	if (!file->open(Samurai::IO::File::Mode::Read)) {
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
	delete handler;

	file->close();

	return ret;
}




