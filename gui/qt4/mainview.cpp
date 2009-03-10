/*
 * Copyright (C) 2001-2008 Jan Vidar Krey, janvidar@extatic.org
 * See the file "COPYING" for licensing details.
 */

#include "mainview.h"
#include "hubview.h"

#include <QListWidget>
#include <QMenu>
#include <QStackedWidget>
#include <QTimer>
#include <QSplitter>
#include <QHttp>
#include <QUrl>
#include <QTextEdit>

#include <QtWebKit>
// #include <QWebNetworkInterface>


class DefaultWidget : public QWebPage
{
	public:
		DefaultWidget(QWidget* parent) : QWebPage(parent)
		{
			// setWindowTitle("Startpage");
		}
		
		void startPage()
		{
			// mainFrame()->load(QUrl("http://localhost/quickdc/"));
		}
		
		/*
		NavigationRequestResponse navigationRequested(QWebFrame *frame, const QWebNetworkRequest &request, NavigationType type)
		{
			(void) frame;
			(void) type;
			
			qDebug("Frame requested to go to URL: %s", request.url().toString().toUtf8().data());
			return IgnoreNavigationRequest;
		}
		*/
};



Chrome::Qt4::MainView::MainView(CoreScheduler& core_) : QMainWindow(), core(core_)
{
	qDebug("MainView: %s", __PRETTY_FUNCTION__);
	
	/*
	(void) statusBar();
	*/
/*	
	QSplitter* splitter = new QSplitter(this);
	splitter->setMinimumSize(QSize(50, 50));
	splitter->setOrientation(Qt::Horizontal);*/
	
	// switcher = new WidgetList(splitter);
	// QTextEdit* windows = new QTextEdit(this);
	windows = new QStackedWidget(this);
	
	/* Add the main widget to stack */
	DefaultWidget* defaultWidget = new DefaultWidget(this);
	defaultWidget->startPage();
	
	
	setCentralWidget(windows);
	resize(QSize(600, 480).expandedTo(minimumSizeHint()));
	
//	addWidget(defaultWidget->view());

}

/*
 *  Destroys the object and frees any allocated resources
 */
Chrome::Qt4::MainView::~MainView()
{
	qDebug("MainView: %s", __PRETTY_FUNCTION__);
}
 
void Chrome::Qt4::MainView::addHub(Samurai::IO::Net::URL* url)
{
	qDebug("MainView: %s", __PRETTY_FUNCTION__);
	
	HubView* hubview = new HubView(url, windows);
	// hubview->setWindowTitle(url->toString());
	
	addWidget(hubview);
	//windows->setCurrentWidget(hubview);
	
	// new WindowPanelItem(leftPanel, hubview);
}

void Chrome::Qt4::MainView::activateWindow(QWidget* window)
{
	qDebug("MainView: %s", __PRETTY_FUNCTION__);
	windows->setCurrentWidget(window);
}

void Chrome::Qt4::MainView::addWidget(QWidget* w)
{
	qDebug("MainView: %s", __PRETTY_FUNCTION__);
	windows->addWidget(w);
	// switcher->add(w);
}

