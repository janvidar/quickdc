/*
 * Copyright (C) 2001-2007 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_GUI_QT4_USERWIDGET_H
#define HAVE_QUICKDC_GUI_QT4_USERWIDGET_H


#include <QListView>
#include <QListWidgetItem>

class QLabel;
class QLineEdit;
class QMenu;

namespace QuickDC {
class User;
}

namespace Chrome {
namespace Qt4 {

class UserListItem : public QListWidgetItem {
	public:
		UserListItem(const QuickDC::User*, QListWidget*);
		virtual ~UserListItem();
		QString key(int column, bool ascending) const;
		/* int compare(QListViewItem * i, int col, bool ascending) const; */
		const QuickDC::User* getUser() const;
		void update();
	private:
		const QuickDC::User* user_ptr;
};

class UserWidget : public QWidget
{
	Q_OBJECT

	public:
		UserWidget( QWidget* parent = 0, const char* name = 0, Qt::WindowFlags fl = 0 );
		~UserWidget();

		void append(const QuickDC::User* user);
		void remove(const QuickDC::User* user);
		void update(const QuickDC::User* user);
		void clear();

	signals:
		virtual void chatUser(const QuickDC::User*);
		virtual void browseUser(const QuickDC::User*);

	public slots:
		void slotUserBrowse();
		void slotUserMessage();
		/* void slotUserPopup(QListViewItem*,const QPoint&,int); */

	private:
		const QuickDC::User* getSelectedUser() const;

	protected:
		
		QLabel* textLabel;
		QLineEdit* userFilter;
		QListWidget* listUsers;
		QMenu* menu;
};

}
}

#endif // HAVE_QUICKDC_GUI_QT3_USERWIDGET_H
