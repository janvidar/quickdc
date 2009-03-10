/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_ADCBBS_STORAGE_H
#define HAVE_QUICKDC_ADCBBS_STORAGE_H

#include "quickdc.h"
#include "network/adc/adccommands.h"
#include "network/adc/cid.h"
#include <map>

namespace ADC {
	
	class BBS_Message {
		public:
			BBS_Message(bbs_message_t id, bbs_message_t parentId, char* cid, char* nick);
			virtual ~BBS_Message();
			
			void setBody(ADC::Command* command);
			Samurai::TimeStamp getTimeStamp();
			
			char* getNick();
			char* getCID();
			char* getSubject();
			char* getBody();
			bbs_message_t getID();
			bbs_message_t getParentID();
			
		private:
			bbs_message_t id;
			bbs_message_t parentId;
			Samurai::TimeStamp timestamp;
			char* cid;
			char* nick;
			enum Flag { Reserved, Active, Closed, Deleted, Sticky, Refused };
			char* subject;
			char* body;
	};
	
	class BBS_Storage {
		public:
			BBS_Storage();
			virtual ~BBS_Storage();
			
			BBS_Message* getMessage(bbs_message_t id);
			
		protected:
			std::map<bbs_message_t, BBS_Message*> messages;
	};

}
#endif // HAVE_QUICKDC_ADCBBS_STORAGE_H
