/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "chatwidget.h"

#include <qvariant.h>
#include <qlineedit.h>
#include <qtextedit.h>
#include <qlayout.h>
#include <qtooltip.h>
#include <qwhatsthis.h>
#include <qregexp.h>

Chrome::Qt4::ChatWidget::ChatWidget(const QuickDC::User* user, QWidget* parent, const char* name, Qt::WindowFlags fl) : QWidget(parent, fl), user_ptr(user)
{
	(void) name;

	QGridLayout* layout = new QGridLayout(this);
	chatLine = new QLineEdit(this);
	layout->addWidget(chatLine, 1, 0);
	chatWindow = new QTextEdit(this);
	
	layout->addWidget(chatWindow, 0, 0);
	resize(QSize(494, 455).expandedTo(minimumSizeHint()));
	
	chatWindow->setReadOnly(true);
	chatWindow->setAcceptRichText(true);
	chatLine->setFocus();

	// signals and slots connections
	connect(chatLine, SIGNAL(returnPressed()), this, SLOT(sendMessage()));
}

Chrome::Qt4::ChatWidget::~ChatWidget()
{
}

void Chrome::Qt4::ChatWidget::sendMessage() {
	QString text = chatLine->text();
	chatLine->setText("");
	chatLine->setFocus();
	if (text == "") return;
	if (user_ptr)
		emit sendPrivateMessage(user_ptr, text);
	else
		emit sendMessage(text);
}

void Chrome::Qt4::ChatWidget::addMessage(const QString& msg) {
	QString text;
	QRegExp rx = QRegExp("^<.+>.*$");

	int p = msg.indexOf('>');
	if ((p != -1) && (rx.exactMatch(msg))) text =  "<b>"  + msg.mid(1, p-1) + "</b>:&nbsp;" + escapeString(msg.mid(p + 2));
	else text = escapeString(msg);

	chatWindow->append(getTimestamp() + text);

#if 0
	// Only keep track of the bottom if we are at the bottom (otherwise no scrolling).
	int para, index;
	chatWindow->getCursorPosition(&para, &index);
	if (para >= (chatWindow->paragraphs() - 1)) chatWindow->setCursorPosition(chatWindow->paragraphs(), 0);
#endif // 0
}

// FIXME: Highlite URLs
QString Chrome::Qt4::ChatWidget::escapeString(const QString& s) {
	QString str = s;
	str.replace(QRegExp("<"), "&lt;");
	str.replace(QRegExp(">"), "&gt;");
	str.replace(QRegExp("\n"), "<br>");
	str.replace(QRegExp("\t"), "&nbsp;&nbsp;&nbsp;&nbsp;");
	return str;
}

QString Chrome::Qt4::ChatWidget::getTimestamp() {
	/*
		config->setGroup("misc");
		return ((config->readBoolEntry("chatTimestamp")) ? QString("<tt>[" + QTime::currentTime().toString() + "]</tt>") : QString(""));
	*/
	return "";
}

void Chrome::Qt4::ChatWidget::addStatusMessage(const QString& text) {
	chatWindow->append("<font color=\"blue\">" + getTimestamp() + "** " + text + "</font>");
}

void Chrome::Qt4::ChatWidget::addErrorMessage(const QString& text) {
	chatWindow->append("<font color=\"red\">" + getTimestamp() + "** " + text + "</font>");
}
