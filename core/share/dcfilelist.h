/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_FILELIST_DECODER_H
#define HAVE_FILELIST_DECODER_H

#include <sys/types.h>
#include <vector>
#include <samurai/io/xml.h>

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

		RemoteFile(const RemoteFile&) = delete;
		RemoteFile& operator=(const RemoteFile&) = delete;

		size_t getSize() const { return size; }

		/** @return the base name, never 0. */
		const char* getName() const { return name; }

		/** @return the TTH root in base32, or 0 if the list did not carry one. */
		const char* getTTH() const { return tth; }

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

		RemoteDir(const RemoteDir&) = delete;
		RemoteDir& operator=(const RemoteDir&) = delete;
		
		uint64_t getSize()  const { return size; }
		uint64_t getFiles() const { return filecount; }

		RemoteDir* getParent() const { return parent; }

		/** @return the directory name, never 0. */
		const char* getName() const { return name; }

		/* Walking the decoded tree. */
		size_t countEntries() const { return files.size(); }
		Share::RemoteFile* getEntry(size_t n) const { return (n < files.size()) ? files[n] : 0; }

		size_t countSubdirs() const { return subdirs.size(); }
		Share::RemoteDir* getSubdir(size_t n) const { return (n < subdirs.size()) ? subdirs[n] : 0; }

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


class XMLFileListHandler : public Samurai::IO::XmlContentHandler {
	public:
		XMLFileListHandler(RemoteDir* root_, Samurai::IO::Buffer* buffer);
		~XMLFileListHandler() override;

		XMLFileListHandler(const XMLFileListHandler&) = delete;
		XMLFileListHandler& operator=(const XMLFileListHandler&) = delete;
		
		bool parse();
	
	public: /* Reported by the reader */
		void startDocument() override;
		void endDocument() override;
		void startElement(std::string_view, std::string_view tag,
			const Attributes& attributes) override;
		void endElement(std::string_view, std::string_view tag) override;
		void characters(std::string_view) override;
		
	protected:
		void onProblem();
	
		/* Borrowed, and read when parse() is called rather than at construction. */
		Samurai::IO::Buffer* source;
		RemoteDir* root;
		RemoteDir* currentDir;
		/* Open <Directory> elements inside a subtree refused for its depth,
		   waiting for their end tags. Nothing in them is built. */
		size_t skipped;
		bool problem;
};


class RemoteFileList {
	public:
		RemoteFileList(Samurai::IO::File* file_);
		virtual ~RemoteFileList();

		RemoteFileList(const RemoteFileList&) = delete;
		RemoteFileList& operator=(const RemoteFileList&) = delete;
	
		bool decode();

	public:
		QuickDC::User* user;
		Samurai::IO::File* file;
		Samurai::IO::Buffer* buffer;
		RemoteDir* root;
};



}


#endif // HAVE_FILELIST_DECODER_H

