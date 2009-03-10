/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include <qwidget.h>
#include <qsplitter.h>
#include <qlayout.h>
#include <qtabwidget.h>
#include <QTextEdit>
#include "hubview.h"

Chrome::Qt4::HubView::HubView(Samurai::IO::Net::URL* url, QWidget* parent, Qt::WindowFlags fl) :
	QWidget(parent, fl)
{
/*
	if (!name) setName("HubView");
*/
	QGridLayout* layout = new QGridLayout(this);
	
	tabs = new QTabWidget(this);

	QSplitter* splitter = new QSplitter(tabs);
	splitter->setMinimumSize(QSize(0, 0));
	splitter->setOrientation(Qt::Horizontal);
	
	chatWidget = new ChatWidget(0, splitter);
	userWidget = new UserWidget(splitter);

	tabs->addTab(splitter, url->toString().c_str()); // FIXME: caption/title?
	layout->addWidget(tabs, 0, 0);
	
	hub = new QuickDC::Hub(this, url);
	hub->connect();

	// Connect the slots related to the chat window.
	connect(chatWidget, SIGNAL(sendMessage(const QString&)), this, SLOT(slotSendMessage(const QString&)));
	connect(userWidget, SIGNAL(chatUser(const QuickDC::User*)), this, SLOT(slotChatTab(const QuickDC::User*)));
}


Chrome::Qt4::HubView::~HubView()
{
	delete hub;
}


void Chrome::Qt4::HubView::setStatusMessage(const QString& msg, bool error) {
	if (error) {
		chatWidget->addErrorMessage(msg);
	} else {
		chatWidget->addStatusMessage(msg);
	}
}

void Chrome::Qt4::HubView::EventHubStatus(enum StatusHub hubstate) {
	switch (hubstate) {
		case HubNone:
			break;

		case HubLogin:
			setStatusMessage(tr("Logging in..."), false);
			break;

		case HubPassword:
			setStatusMessage(tr("Password required..."), false);
			break;

		case HubLoggedIn:
			setStatusMessage(tr("Logged in..."), false);
			break;

		case HubErrorLoginRefused:
			setStatusMessage(tr("Login failed!"), true);
			break;

		case HubErrorDisabled:
			setStatusMessage(tr("Hub is disabled"), true);
			break;

		case HubErrorNickTaken:
			setStatusMessage(tr("Nickname is taken"), true);
			break;

		case HubErrorCIDTaken:
			setStatusMessage(tr("You are already logged in!"), true);
			break;

		case HubBannedTemporarily:
			setStatusMessage(tr("You are banned."), true);
			break;

		case HubBannedPermanently:
			setStatusMessage(tr("You are permanently banned."), true);
			break;

		case HubErrorFull:
			setStatusMessage(tr("Hub is full"), true);
			break;

		case HubErrorNickNotAccepted:
			setStatusMessage(tr("Error: Nick name is not accepted"), true);
			break;

		case HubErrorWrongPassword:
			setStatusMessage(tr("Wrong password"), true);
			break;

		case HubKicked:
			setStatusMessage(tr("Kicked"), true);
			break;

		case HubRedirected:
			setStatusMessage(tr("Redirecting..."), false);
			break;
	}
}


void Chrome::Qt4::HubView::EventNetStatus(enum StatusNetwork netstate)
{
	switch (netstate) {
		case NetInvalid:
			setStatusMessage(tr("Invalid network address"), true);
			break;

		case NetLookup:
			setStatusMessage(tr("Looking up host..."), false);
			break;
			
		case NetConnecting:
			setStatusMessage(tr("Connecting..."), false);
			break;

		case NetConnected:
			setStatusMessage(tr("Connected."), false);
			break;

		case NetDisconnected:
			setStatusMessage(tr("Disconnected."), false);
			break;

		case NetTlsConnected:
			setStatusMessage(tr("Connection is secure."), false);
			break;

		case NetTlsDisconnected:
			setStatusMessage(tr("Connection is not secure any longer."), false);
			break;

		case NetErrorTimeout:
			setStatusMessage(tr("Error: Connection timed out."), true);
			break;

		case NetErrorConnectionRefused:
			setStatusMessage(tr("Error: Connection refused."), true);
			break;

		case NetErrorHostNotFound:
			setStatusMessage(tr("Error: Connection refused."), true);
			break;

		case NetErrorIO:
			setStatusMessage(tr("Error: An IO error occured."), true);
			break;

		case NetTlsError:
			setStatusMessage(tr("Error: A secure socket error occured."), true);
			break;
	}
}


void Chrome::Qt4::HubView::EventChat(const QuickDC::User* user, const char* message, bool)
{
	QString q_message = QString::fromUtf8(user->getNick());
	q_message += ": ";
	q_message += QString::fromUtf8(message);
	chatWidget->addMessage(q_message);
}


void Chrome::Qt4::HubView::EventPrivateChat(const QuickDC::User* from, const QuickDC::User* to, const char* message, const char* /* context */, bool /*action */)
{
	const QuickDC::User* context = 0;
	if (from == hub->getLocalUser()) {
		context = to;
	} else {
		context = from;
	}
	
	Chrome::Qt4::ChatWidget* widget = getChatWidget(context);
	if (!widget) widget = createChatWidget(context);

	QString q_message = QString::fromUtf8(from->getNick());;
	q_message += ": ";
	q_message += QString::fromUtf8(message);
	widget->addMessage(q_message);
}


void Chrome::Qt4::HubView::EventHubMessage(const char* user, const char* message)
{
	QString q_message = user;
	q_message += ": ";
	q_message += QString::fromUtf8(message);
	chatWidget->addMessage(q_message);
}


void Chrome::Qt4::HubView::EventUserJoin(const char*)
{
//	chatWidget->addMessage(QString::fromUtf8(user) + " joined");
}


void Chrome::Qt4::HubView::EventUserLeave(const QuickDC::User* user, const char*, bool)
{
/*
	QString txt;
	if (message)
		txt = QString(tr("%1 left (%2)")).arg(QString::fromUtf8(user)).arg(QString::fromUtf8(message));
	else
		txt = QString(tr("%1 left")).arg(QString::fromUtf8(user));
	chatWidget->addMessage(txt);
*/
	userWidget->remove(user);
}

void Chrome::Qt4::HubView::EventUserUpdate(const QuickDC::User* user)
{
	userWidget->update(user);
}

void Chrome::Qt4::HubView::EventUsersCleanup() {
	userWidget->clear();
}

void Chrome::Qt4::HubView::EventSearchResult(void*)
{

}

void Chrome::Qt4::HubView::EventHubName(const char* hubname)
{
	chatWidget->addMessage(QString(tr("hub name: ")) + hubname);
}

bool Chrome::Qt4::HubView::EventHubRedirect(const char*, uint16_t)
{
	return false;
}

void Chrome::Qt4::HubView::EventHubAutenticate()
{

}

bool Chrome::Qt4::HubView::EventClientConnect(const char*, uint16_t, const QuickDC::User*)
{
	return false;
}

bool Chrome::Qt4::HubView::EventClientConnect(const QuickDC::User*)
{
	return false;
}

void Chrome::Qt4::HubView::slotSendMessage(const QString& msg) {
	hub->sendChatMessage(msg.toUtf8());
}

void Chrome::Qt4::HubView::slotSendPrivateMessage(const QuickDC::User* user, const QString& msg) {
	hub->sendPrivateChatMessage(user, msg.toUtf8());
}

Chrome::Qt4::ChatWidget* Chrome::Qt4::HubView::getChatWidget(const QuickDC::User* user) {
	Chrome::Qt4::ChatWidget* widget = 0;
	for (int n = 0; n < tabs->count(); n++) {
		widget = dynamic_cast<Chrome::Qt4::ChatWidget*>(tabs->widget(n));
		if (widget && widget->getUser() == user)
			return widget;
	}
	return 0;
}

Chrome::Qt4::ChatWidget* Chrome::Qt4::HubView::createChatWidget(const QuickDC::User* user) {
	Chrome::Qt4::ChatWidget* widget = new ChatWidget(user, tabs, "ChatWidget");
	tabs->addTab(widget, QString::fromUtf8(user->getNick()));

	connect(widget, SIGNAL(sendPrivateMessage(const QuickDC::User*, const QString&)),
		this, SLOT(slotSendPrivateMessage(const QuickDC::User*, const QString&)));

	return widget;
}

// FIXME: Assign icon.
void Chrome::Qt4::HubView::slotChatTab(const QuickDC::User* user) {
	Chrome::Qt4::ChatWidget* widget = getChatWidget(user);
	if (!widget) {
		widget = createChatWidget(user);
	} else {
		/* Update it */
	}
}

