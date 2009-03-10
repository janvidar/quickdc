/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "widgetlist.h"

#include <QWidget>
#include <QMouseEvent>
#include <QLabel>
#include <QBoxLayout>
// #include <QAbstractItemView>
 
Chrome::Qt4::WidgetListItem::WidgetListItem(Chrome::Qt4::WidgetList* list, QWidget* widget_) : QLabel(widget_->windowTitle(), list), widget(widget_)
		{
			// setOpenExternalLinks(true);
			// setTextFormat(Qt::RichText);
			// QLabel* label = new QLabel(this);
			// QString text = QString(tr("Hub:")) + QString("<B>") + widget->windowTitle() + QString("</B>");
			// label->setText(text);
			
		}
		
Chrome::Qt4::WidgetListItem::~WidgetListItem()
{
			
}
		
void Chrome::Qt4::WidgetListItem::mouseReleaseEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton)
	{
		event->accept();
		qDebug("Clicked to activate");
	}
}

/*
void Chrome::Qt4::WidgetList::mouseReleaseEvent(QMouseEvent* event)
{
	qDebug("mouseReleaseEvent()");
}

void Chrome::Qt4::WidgetList::mousePressEvent(QMouseEvent* event)
{
	qDebug("mousePressEvent()");
}
*/

Chrome::Qt4::WidgetList::WidgetList(QWidget* parent) : QWidget(parent)
{
	layout = new QBoxLayout(QBoxLayout::TopToBottom, this);
	layout->setObjectName("Chrome::Qt4::WidgetList::WidgetList::layout");
	setLayout(layout);
	setMinimumSize(140, 100);
}

Chrome::Qt4::WidgetList::~WidgetList()
{

}

void Chrome::Qt4::WidgetList::add(QWidget* widget)
{
	WidgetListItem* item = new WidgetListItem(this, widget);
	//layout->addWidget((QWidget*) item);
	widgets.append(item);
}


void Chrome::Qt4::WidgetList::remove(QWidget* widget)
{
	for (QList<WidgetListItem*>::iterator i = widgets.begin(); i != widgets.end(); ++i)
	{
		WidgetListItem* item = (*i);
		if (item && item->getWidget() == widget)
		{
			widgets.erase(i);
			break;
		}
	}
}


