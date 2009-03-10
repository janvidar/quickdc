/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "quickdc.h"
#include "network/adc/adccommon.h"

namespace ADC {


class CID {
	public:
		static CID* getInstance();
		
		virtual ~CID();

		char* getCID() {
			return cid;
		}
		
		char* getPID() {
			return pid;
		}

		static bool verify(const char* cid);
		
		static bool verifyCIDandPID(const char* cid, const char* pid);
		
	protected:
		CID();
		
	private:
		void generate();
		bool load();
		bool store();
		
	protected:
		char* cid;
		char* pid;
};

class SID {
	public:
		static const char* toString(sid_t);
		static sid_t       fromString(const char*);
};

extern CID* global_cid;


}

