/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONFIG_BACKEND_H
#define HAVE_QUICKDC_CONFIG_BACKEND_H

#include <string>

namespace Samurai {
	namespace IO {
		class File;
		class Buffer;
	}
}

namespace QuickDC {

class Backend {
	public:
		Backend(const std::string& file);
		virtual ~Backend();
	

	protected:
		/**
		 * Call this before committing any changes.
		 */
		virtual void writeStart() = 0;

		/**
		 * Call this after all changes are commited.
		 */
		virtual void writeEnd() = 0;
		
		/**
		 * This will start a section of settings.
		 */
		virtual void writeSectionStart(const std::string& group) = 0;

		/**
		 * This will end a section of settings.
		 */
		virtual void writeSectionEnd(const std::string& group) = 0;

		/**
		 * This is a setting.
		 */
		virtual void writeSetting(const std::string& name, const std::string& value) = 0;

		/**
		 * This is called when a section starts.
		 */
		virtual void readSectionStart(const std::string& group) = 0;

		/**
		 * This is called when a section ends.
		 */
		virtual void readSectionEnd(const std::string& group) = 0;

		/**
		 * This is called when a setting occurs.
		 */
		virtual void readSetting(const std::string& name, const std::string& value) = 0;

	
		virtual bool write();
		virtual bool read();

	protected:
		Samurai::IO::Buffer* buffer;
		Samurai::IO::File* file;
	
};

}

#endif // HAVE_QUICKDC_CONFIG_BACKEND_H
