/*
 * Copyright (C) 2001-2006 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */


#include <quickdc.h>
#include <network/user.h>
#include <samurai/util/format.h>
#include "userwidget.h"

#include <QLayout>
#include <QLabel>
#include <QLineEdit>

#define USERLIST_COL_USER        0
#define USERLIST_COL_SHARESIZE   1
#define USERLIST_COL_DESCRIPTION 2
#define USERLIST_COL_EMAIL       3
#define USERLIST_COL_CONNECTION  4
#define USERLIST_COL_USERAGENT   5
#define USERLIST_COL_FLAGS       6

extern const char* getFormatSize(uint64_t size);

Chrome::Qt4::UserListItem::UserListItem(const QuickDC::User* user, QListWidget* parent) : QListWidgetItem(parent), user_ptr(user)
{
	update();
	user_ptr->ptr_data = this;
}

void Chrome::Qt4::UserListItem::update()
{

	setText(/*USERLIST_COL_USER, */QString::fromUtf8(user_ptr->getNick()));
	/*
	setText(USERLIST_COL_SHARESIZE, getFormatSize(user_ptr->getSharedBytes()));
	setText(USERLIST_COL_DESCRIPTION, QString::fromUtf8(user_ptr->getDescription()));
	setText(USERLIST_COL_EMAIL, QString::fromUtf8(user_ptr->getEmail()));
	setText(USERLIST_COL_USERAGENT, QString::fromUtf8(user_ptr->getUserAgent()));
	setText(USERLIST_COL_CONNECTION, getFormatSize(user_ptr->getMaxSpeedUpload()));
	*/
}

Chrome::Qt4::UserListItem::~UserListItem()
{
	user_ptr->ptr_data = 0;
}

/*
int Chrome::Qt4::UserListItem::compare(QListViewItem* i, int col, bool ascending) const
{
	Chrome::Qt4::UserListItem* u = dynamic_cast<Chrome::Qt4::UserListItem*>(i);

	switch (col) {
		case USERLIST_COL_SHARESIZE:
			return (u->user_ptr->getSharedBytes() < user_ptr->getSharedBytes()) ? -1 : 1;
		default:
			return key(col, ascending).compare(i->key(col, ascending));
	}
	return key(col, ascending).compare(i->key(col, ascending));	
}
*/

QString Chrome::Qt4::UserListItem::key(int column, bool ascending) const
{
	(void) column;
	(void) ascending;
	
//	if (column == USERLIST_COL_USER) {
		// Sort operators separately from regular users
		QString nick = QString::fromUtf8(user_ptr->getNick()).toLower();
		return (user_ptr->isOp()) ? QString("A" + nick) : QString("B" + nick);
//	}
}

const QuickDC::User* Chrome::Qt4::UserListItem::getUser() const
{
	return user_ptr;
}



Chrome::Qt4::UserWidget::UserWidget(QWidget* parent, const char* name, Qt::WindowFlags fl) : QWidget(parent, fl)
{
	(void) name;
// 	if (!name)
// 		setName("UserWidget");

	const size_t minimum_width = 75;
	const size_t default_width = 150;
	
	setMinimumSize(QSize(minimum_width, 0));
	
	QWidget* filterContainer = new QWidget(this);
	QHBoxLayout* hlayout = new QHBoxLayout(filterContainer);
	
	textLabel = new QLabel(this);
	// textLabel->setSizePolicy(QSizePolicy((QSizePolicy::SizeType) 1, (QSizePolicy::SizeType) 5, 0, 0, textLabel->sizePolicy().hasHeightForWidth()));
	textLabel->setText(tr("Filter:"));
	hlayout->addWidget(textLabel);
	
	userFilter = new QLineEdit(filterContainer);
	// userFilter->setSizePolicy(QSizePolicy((QSizePolicy::SizeType) 3, (QSizePolicy::SizeType) 0, 0, 0, userFilter->sizePolicy().hasHeightForWidth()));
	userFilter->setMinimumSize(QSize(minimum_width, 0));
	hlayout->addWidget(userFilter);
	
	QVBoxLayout* layout = new QVBoxLayout(this);
	listUsers = new QListWidget(this);
	
	/*
	listUsers->addColumn("");
	listUsers->addColumn("");
	listUsers->addColumn("");
	listUsers->addColumn("");
	listUsers->addColumn("");
	listUsers->addColumn("");
	listUsers->addColumn("");
	
	listUsers->setColumnText(USERLIST_COL_USER,        tr("User"));
	listUsers->setColumnText(USERLIST_COL_CONNECTION,  tr("Connection"));
	listUsers->setColumnText(USERLIST_COL_SHARESIZE,   tr("Shared"));
	listUsers->setColumnText(USERLIST_COL_DESCRIPTION, tr("Description"));
	listUsers->setColumnText(USERLIST_COL_USERAGENT,   tr("User-Agent"));
	listUsers->setColumnText(USERLIST_COL_EMAIL,       tr("E-mail"));
	listUsers->setColumnText(USERLIST_COL_FLAGS,       tr("Flags"));
	listUsers->setAllColumnsShowFocus(TRUE);
	*/
	
	layout->addWidget(listUsers);
	layout->addWidget(filterContainer);
	
	resize(QSize(default_width, 0).expandedTo(minimumSizeHint()));
	
	// signals and slots connections
	connect(listUsers, SIGNAL(rightButtonClicked(QListViewItem*,const QPoint&,int)), this, SLOT( slotUserPopup(QListViewItem*,const QPoint&,int)));

	menu = 0;
}

/*
 *  Destroys the object and frees any allocated resources
 */
Chrome::Qt4::UserWidget::~UserWidget()
{

}

void Chrome::Qt4::UserWidget::slotUserBrowse()
{
	const QuickDC::User* user = getSelectedUser();
	if (user)
	{
		emit browseUser(user);
	}
}

void Chrome::Qt4::UserWidget::slotUserMessage()
{
	const QuickDC::User* user = getSelectedUser();
	if (user)
	{
		emit chatUser(user);
	}
}

const QuickDC::User* Chrome::Qt4::UserWidget::getSelectedUser() const
{
/*
	UserListItem* item = dynamic_cast<UserListItem*>(listUsers->selectedItem());
	if (item)
		return item->getUser();*/
	return 0;
}

/*
void Chrome::Qt4::UserWidget::slotUserPopup(QListViewItem* item, const QPoint& pos, int column )
{
	(void) column;
	
	if (item) {
		if (menu) menu->clear();
		else menu = new QPopupMenu(this, "User popup menu");
		menu->insertItem(tr("Send message..."), this, SLOT(slotUserMessage()));
		menu->insertItem(tr("Browse user..."), this, SLOT(slotUserBrowse()));
		menu->popup(pos);
	}
}
*/

void Chrome::Qt4::UserWidget::append(const QuickDC::User* user)
{
	/* UserListItem* item = */ new UserListItem(user, listUsers);
}

void Chrome::Qt4::UserWidget::remove(const QuickDC::User* user)
{
	if (user->ptr_data) {
		UserListItem* item = (UserListItem*) user->ptr_data;
		delete item;
	}
}

void Chrome::Qt4::UserWidget::clear()
{
	listUsers->clear();
}

void Chrome::Qt4::UserWidget::update(const QuickDC::User* user)
{
	if (!user->ptr_data) {
		append(user);
	} else {
		UserListItem* item = (UserListItem*) user->ptr_data;
		item->update();
	}
}
