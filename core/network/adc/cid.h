/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCCID_H
#define HAVE_QUICKDC_ADCCID_H

#include "quickdc.h"
#include "network/adc/adccommon.h"

#include <string>
#include <string_view>

namespace QuickDC { class Preferences; }

namespace ADC {


class CID {
	public:
		explicit CID(QuickDC::Preferences& config);

		virtual ~CID();

		/**
		 * @return the client ID, or null when this client has no identity -
		 *         which happens only if none could be generated securely.
		 *         Owned by the CID and valid for as long as it is.
		 */
		const char* getCID() const {
			return cid.empty() ? 0 : cid.c_str();
		}

		/** The private ID behind getCID(), with the same contract. */
		const char* getPID() const {
			return pid.empty() ? 0 : pid.c_str();
		}

		static bool verify(const char* cid);

		static bool verifyCIDandPID(const char* cid, const char* pid);


	private:
		void generate();
		bool load();
		bool store();

	protected:
		/*
		 * Both are 39 base32 characters when there is an identity at all, so an
		 * empty one is no identity rather than a short one.
		 */
		QuickDC::Preferences& config;
		std::string cid;
		std::string pid;
};

class SID {
	public:
		/**
		 * NOTE: the answer lives in one buffer per thread, so it is only good
		 * until the next call on that thread.
		 */
		static const char* toString(sid_t);
		static sid_t       fromString(const char*);
};



}

#endif // HAVE_QUICKDC_ADCCID_H
