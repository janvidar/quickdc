/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_CONFIG_INIPARSER_H
#define HAVE_QUICKDC_CONFIG_INIPARSER_H

#include "backend.h"
#include <map>
#include <string>

namespace QuickDC {

class IniParser : public Backend {
	public:
		IniParser(const std::string& file);
		virtual ~IniParser();

	protected:
		/**
		 * Call this before committing any changes.
		 */
		virtual void writeStart() override;

		/**
		 * Call this after all changes are commited.
		 */
		virtual void writeEnd() override;

		/**
		 * This will start a section of settings.
		 */
		virtual void writeSectionStart(const std::string& group) override;

		/**
		 * This will end a section of settings.
		 */
		virtual void writeSectionEnd(const std::string& group) override;

		/**
		 * This is a setting.
		 */
		virtual void writeSetting(const std::string& name, const std::string& value) override;

		/**
		 * This is called when a section starts.
		 */
		virtual void readSectionStart(const std::string& group) override = 0;

		/**
		 * This is called when a section ends.
		 */
		virtual void readSectionEnd(const std::string& group) override = 0;

		/**
		 * This is called when a setting occurs.
		 */
		virtual void readSetting(const std::string& name, const std::string& value) override = 0;
		
	protected:	
		virtual bool write() override;
		virtual bool read() override;



};

}

#endif // HAVE_QUICKDC_CONFIG_INIPARSER_H
