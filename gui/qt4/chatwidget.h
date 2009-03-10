/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_GUI_QT4_CHATWIDGET_H
#define HAVE_QUICKDC_GUI_QT4_CHATWIDGET_H

#include <qvariant.h>
#include <qwidget.h>

class QGridLayout;
class QSpacerItem;
class QLineEdit;
class QTextEdit;

namespace QuickDC {
	class User;
}

namespace Chrome {
namespace Qt4 {

class ChatWidget : public QWidget
{
	Q_OBJECT

	public:
		ChatWidget(const QuickDC::User* user, QWidget* parent = 0, const char* name = 0, Qt::WindowFlags fl = Qt::Widget);
		~ChatWidget();
		
		const QuickDC::User* getUser() const { return user_ptr; }

	signals:
		void sendMessage(const QString& msg);
		void sendPrivateMessage(const QuickDC::User* user, const QString& msg);

	public slots:
		void sendMessage();
		void addMessage(const QString& msg);
		void addStatusMessage(const QString& msg);
		void addErrorMessage(const QString& msg);

	protected:
		QString escapeString(const QString& s);
		QString getTimestamp();

	protected:
		QLineEdit* chatLine;
		QTextEdit* chatWindow;
		const QuickDC::User* user_ptr;
};

}
}

#endif // HAVE_QUICKDC_GUI_QT4_CHATWIDGET_H
