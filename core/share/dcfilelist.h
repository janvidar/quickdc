/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_FILELIST_DECODER_H
#define HAVE_FILELIST_DECODER_H

#include <sys/types.h>
#include <vector>
#include "xml/reader.h"

namespace Samurai {
	namespace IO {
		class File;
		class Buffer;
	}
}

namespace QuickDC {
	class User;
}

namespace Share {

class RemoteFileList;
class RemoteDir;
class RemoteFile;

class RemoteFile {
	public:
		RemoteFile(const char* name_, uint64_t size_, const char* tth_, RemoteDir* parent_);
		virtual ~RemoteFile();

		size_t getSize() const { return size; }
		
	protected:
		char* name;
		char* tth;
		uint64_t size;
		RemoteDir* parent;
};

class RemoteDir {
	public:
		RemoteDir(const char* name_, RemoteDir* parent_ = 0);
		virtual ~RemoteDir();
		
		uint64_t getSize()  const { return size; }
		uint64_t getFiles() const { return filecount; }
		
		RemoteDir* getParent() const { return parent; }
		
	protected:
		void addSize(uint64_t size_);
		
		Share::RemoteDir* parent;
		std::vector<Share::RemoteDir*> subdirs;
		std::vector<Share::RemoteFile*> files;
		char* name;
		uint64_t size;
		size_t filecount;
		
		friend class RemoteFile;
};


class XMLFileListHandler : public XML::SAX::ContentHandler {
	public:
		XMLFileListHandler(RemoteDir* root_, Samurai::IO::Buffer* buffer);
		virtual ~XMLFileListHandler();
		
		bool parse();
	
	public: /* For SAX parser */
		void startDocument();
		void endDocument();
		void startElement(const char*, const char* localName, const char*, AttributeList list);
		void endElement(const char*, const char* localName, const char*);
		void characters(const char* ch, size_t, size_t);
		void ignorableWhitespace(const char*, size_t, size_t);
		void warning(const char* str);
		void error(const char* str);
		void fatalError(const char*);
		
	protected:
		void onProblem();
	
		XML::SAX::XMLReader* reader;
		RemoteDir* root;
		RemoteDir* currentDir;
		bool problem;
};


class RemoteFileList {
	public:
		RemoteFileList(Samurai::IO::File* file_);
		virtual ~RemoteFileList();
	
		bool decode();

	public:
		QuickDC::User* user;
		Samurai::IO::File* file;
		Samurai::IO::Buffer* buffer;
		RemoteDir* root;
};



}


#endif // HAVE_FILELIST_DECODER_H

