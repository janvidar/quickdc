/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_GUI_QT4_HUBVIEW_H
#define HAVE_QUICKDC_GUI_QT4_HUBVIEW_H


#include <qobject.h>
#include <qwidget.h>
#include <vector>

#include "chatwidget.h"
#include "userwidget.h"

#include <api/hub.h>
#include <api/core.h>
#include <samurai/io/net/url.h>

class QPopupMenu;
class QTabWidget;

namespace Chrome {

namespace Qt4 {

class HubView :
	public QWidget,
	public QuickDC::HubListener
{
	Q_OBJECT

	public:
		HubView(Samurai::IO::Net::URL* url, QWidget* parent = 0, Qt::WindowFlags fl = 0);
    		virtual ~HubView();

	public:
		void EventNetStatus(enum StatusNetwork netstate);
		void EventHubStatus(enum StatusHub hubstate);
		void EventChat(const QuickDC::User* user, const char* message, bool);
		void EventPrivateChat(const QuickDC::User* from, const QuickDC::User* to, const char* message, const char* context, bool action);
		void EventHubMessage(const char* user, const char* message);
		void EventUserJoin(const char* user);
		void EventUserLeave(const QuickDC::User* user, const char* message, bool);
		void EventUserUpdate(const QuickDC::User*);
		void EventUsersCleanup();
		void EventSearchResult(void*);
		void EventHubName(const char* hubname);
		bool EventHubRedirect(const char*, uint16_t);
		void EventHubAutenticate();
		bool EventClientConnect(const char* addr, uint16_t port, const QuickDC::User* u);
		bool EventClientConnect(const QuickDC::User* u);
		
	protected slots:
		void slotSendMessage(const QString& msg);
		void slotSendPrivateMessage(const QuickDC::User* user, const QString& msg);
		void setStatusMessage(const QString& msg, bool error);
		void slotChatTab(const QuickDC::User* user);

	protected:
		ChatWidget* getChatWidget(const QuickDC::User*);
		ChatWidget* createChatWidget(const QuickDC::User*);

		QTabWidget* tabs;
		ChatWidget* chatWidget;
		UserWidget* userWidget;
		QuickDC::Hub* hub;

	protected:

		QPopupMenu *searchMenu;
		QPopupMenu *tabmenu;
		QPopupMenu *tabmenuPublic;
		QPopupMenu *tabmenuSearch;
		QPopupMenu *userCommandHub;
};

}
}

#endif // HAVE_QUICKDC_GUI_QT4_HUBVIEW_H

