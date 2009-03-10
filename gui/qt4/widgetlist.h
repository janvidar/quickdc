/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#ifndef HAVE_QUICKDC_GUI_QT4_WIDGETLIST_H
#define HAVE_QUICKDC_GUI_QT4_WIDGETLIST_H

#include <QWidget>
#include <QList>
#include <QLabel>

class QBoxLayout;

namespace Chrome {
namespace Qt4 {

class WidgetListItem;

class WidgetList : public QWidget
{
    Q_OBJECT
	
	public:
		WidgetList(QWidget* parent);
		~WidgetList();
		
	public:
		void add(QWidget* widget);
		void remove(QWidget* widget);
		

	signals:
		virtual void activateWindow(QWidget*);
		
	protected:
		QBoxLayout* layout;
		QList<WidgetListItem*> widgets;
};


class WidgetListItem : public QLabel
{
	public:
		WidgetListItem(Chrome::Qt4::WidgetList*, QWidget* ptr);
		virtual ~WidgetListItem();
		QWidget* getWidget() const { return widget; }
		
	protected:
		void mouseReleaseEvent(QMouseEvent* event);

	protected:
		QWidget* widget;
};


}
}
#endif // HAVE_QUICKDC_GUI_QT4_WIDGETLIST_H
