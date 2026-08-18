/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "share/dcfilelist.h"
#include "network/downloadqueue.h"
#include <string_view>
#include <vector>
#include <samurai/io/xml.h>
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
	: source(buffer)
	, root(root_)
	, currentDir(0)
	, skipped(0)
	, problem(false)
{
}

Share::XMLFileListHandler::~XMLFileListHandler() {
}

bool Share::XMLFileListHandler::parse() {
	problem = false;
	if (!source) return false;

	/*
	 * A file list is not a small document, and the reader's own defaults are set
	 * for one that is - so every bound is named here against what a share can
	 * legitimately be rather than inherited.
	 *
	 * The size is the same cap the transfer was already held to, so a list that
	 * arrived is a list that can be read. The depth matches the one this handler
	 * enforces on the tree it builds, plus the <FileListing> around it. An
	 * element is a directory or a file, so the count is what bounds a share; the
	 * attributes per element are Name, Size and TTH with room to spare; and the
	 * only character data a file list carries is the whitespace it is laid out
	 * with, which is measured one run at a time rather than added up.
	 */
	Samurai::IO::XmlReader::Limits limits;
	limits.maxDocumentSize = QUICKDC_FILELIST_MAX_PLAIN;
	limits.maxDepth        = QUICKDC_FILELIST_MAX_DEPTH + 2;
	limits.maxElements     = QUICKDC_FILELIST_MAX_ENTRIES;
	limits.maxAttributes   = 16;
	limits.maxTextLength   = 4096;

	const Samurai::IO::XmlError parsed = Samurai::IO::XmlReader::parse(
		std::string_view(source->ptr(), source->size()), *this, limits);

	if (parsed != Samurai::IO::XmlError::Ok)
	{
		QERR("File list refused: %s", Samurai::IO::toString(parsed));
		onProblem();
	}

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

/*
 * localName is whatever name the parser found, and a parser is free to hand a
 * handler none at all: a view over a null pointer has no length to read, so an
 * absent name is turned into the empty one here rather than compared as one.
 */
void Share::XMLFileListHandler::startElement(std::string_view, std::string_view tag,
	const Attributes& attributes) {
	const std::string_view element = tag;
	const bool directory = Samurai::Util::iequals(element, "Directory");

	/* Inside a subtree refused for its depth. Nothing in it is built, and the
	   directories in it are counted so that their end tags close them rather
	   than the one the list was cut off at. */
	if (skipped) {
		if (directory) skipped++;
		return;
	}

	if (Samurai::Util::iequals(element, "FileListing")) {
		if (currentDir) onProblem();
		currentDir = root;
	} else if (directory) {
		if (!currentDir) { onProblem(); return; }
		// Add Directory to the currentDir.

		const char* name = 0;
		for (size_t n = 0; n < attributes.size(); n++) {
			if (Samurai::Util::iequals(attributes[n].first, "Name"))
				name = attributes[n].second.c_str();
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
	} else if (Samurai::Util::iequals(element, "File")) {
		if (!currentDir) { onProblem(); return; }
		const char* name = 0;
		const char* tth = 0;
		uint64_t size = 0;
		
		for (size_t n = 0; n < attributes.size(); n++) {
			if (Samurai::Util::iequals(attributes[n].first, "Name"))
				name = attributes[n].second.c_str();
			else if (Samurai::Util::iequals(attributes[n].first, "TTH"))
				tth = attributes[n].second.c_str();
			else if (Samurai::Util::iequals(attributes[n].first, "Size"))
				size = Samurai::Util::Convert::to_uint64(attributes[n].second.c_str());
		}
		if (name && *name && tth && *tth)
			new Share::RemoteFile(name, size, tth, currentDir);
	}
}

void Share::XMLFileListHandler::endElement(std::string_view, std::string_view tag) {
	const std::string_view element = tag;

	if (Samurai::Util::iequals(element, "Directory")) {
		if (skipped) { skipped--; return; }
		if (!currentDir) { onProblem(); return; }
		currentDir = currentDir->getParent();
	}
}

void Share::XMLFileListHandler::characters(std::string_view)
{
}

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
	size_t size = static_cast<size_t>(file->size());
	if (!file->open(Samurai::IO::File::Mode::Read)) {
		QERR("Unable to open file");
		return false;
	}

	uint8_t* temp_buffer = new uint8_t[size];
	file->read(reinterpret_cast<char*>(temp_buffer), size);
	buffer = new Samurai::IO::Buffer(size);
	buffer->append(reinterpret_cast<char*>(temp_buffer), size);
	delete[] temp_buffer;
	
	XMLFileListHandler* handler = new XMLFileListHandler(root, buffer);
	bool ret = handler->parse();
	delete handler;

	file->close();

	return ret;
}




