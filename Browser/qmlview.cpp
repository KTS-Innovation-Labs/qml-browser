/****************************************************************************
*
* QmlBrowser - Web browser with QML page support
* Copyright (C) 2022 Denis Solomatin <toorion@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
****************************************************************************/
#include "qmlview.h"
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickView>
#include <QSplitter>
#include <QQmlApplicationEngine>
#include <QImage>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QByteArray>
#include "mainwindow.h"
#include "downloadmanagerwidget.h"
#include "browser.h"
#include "networkaccessmanagerfactory.h"
#include <QVBoxLayout>
#include <QQuick3D>
#include <QApplication>
#include "apppaths.h"

QmlView::QmlView(QSplitter *splitter, QWidget *parent)
    : QWidget(parent)
    , BaseView(splitter)
{
    m_parent = parent;

    m_quickView = new QQuickView(&m_qmlEngine, qobject_cast<QWindow*>(window()));
    m_quickView->setResizeMode(QQuickView::SizeRootObjectToView);

    // Required for quick3d, but conflict with some datavisualisation which depends on OpenGL 2.1
    m_quickView->setFormat(QQuick3D::idealSurfaceFormat());

    m_quickView->engine()->setNetworkAccessManagerFactory(NetworkAccessManagerFactory::instance());

    m_container = QWidget::createWindowContainer(m_quickView, this, Qt::Widget);

    auto layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_container);
    setLayout(layout);

    m_api = new ApiWeb(qobject_cast<HttpManager*>(m_quickView->engine()->networkAccessManager()));

    DownloadManagerWidget *downloadManager = (qobject_cast<MainWindow*>(window()))->downloadManagerWidget();
    m_api->qi()->setDownloadManagerWidget(downloadManager);

    QObject::connect(m_quickView->engine(), &QQmlApplicationEngine::warnings, this, [=] (const QList<QQmlError> &warnings) mutable {
        foreach (const QQmlError &error, warnings) {
            m_api->console()->error(error.toString());
        }
    });

    connect(this, &QmlView::titleChanged, dynamic_cast<TabView*>(parent), &TabView::titleChanged);
    connect(this, &QmlView::iconChanged, dynamic_cast<TabView*>(parent), &TabView::iconChanged);
    connect(this, &QmlView::loadFinished, dynamic_cast<TabView*>(parent), &TabView::tabLoadFinished);
}


QmlView::~QmlView()
{
    if(m_contentItem) {
        deepClean(m_contentItem->children());
    }
}


void QmlView::setContent(const QByteArray &content, const QString &mimeType, const QUrl &baseUrl)
{
    (void)mimeType;
    m_url = baseUrl;

    m_api->setLocationUrl(m_url);

    m_component = new QQmlComponent(m_quickView->engine());
    connect(m_component, &QQmlComponent::statusChanged, this, &QmlView::continueLoad);
    m_component->setData(content, baseUrl);

    if (!m_component->errors().isEmpty()) {
        foreach (const QQmlError &error, m_component->errors()) {
            m_api->console()->error(error.toString());
        }
    }
}




void QmlView::continueLoad()
{
    if(m_component->status() == QQmlComponent::Ready) {

        // set LocalStorage path per host
        m_qmlEngine.setOfflineStoragePath(AppPaths::webAppPath(m_url));

        m_context = new QQmlContext(m_quickView->engine()->rootContext());

        AccessRights *accessRights = new AccessRights();
        if(m_url.scheme() == INTERNAL_URL_SCHEME) {
            accessRights->allowAllInternalAccess();
        }

        m_api->setAccessRights(accessRights);

        connect(m_api->window(), &Window::newWindowRequested, [=] (const QUrl &url) {
            qobject_cast<TabView*>(mainBrowser.createWindow()->tabWidget->currentWidget())->setUrl(url);
        });

        connect(m_api->window(), &Window::newTabRequested, this, [=] (const QUrl &url) {
            MainWindow *mainWindow = qobject_cast<MainWindow*>(window());
            mainWindow->tabWidget->createActiveTab()->setUrl(url);
        });

        connect(m_api->locationUrl(), &LocationUrl::urlChanged, this, &QmlView::urlChanged);

        m_context->setContextObject(m_api);

        QObject *component = m_component->beginCreate(m_context);
        m_contentItem = qobject_cast<QQuickItem*>(component);
        if(!m_contentItem) {
            m_api->console()->error(tr("Wrong root object type [%1], instance of Item awaiting").arg(component->metaObject()->className()));
            return;
        }
        m_component->completeCreate();

        m_quickView->engine()->setObjectOwnership(m_component, QQmlEngine::CppOwnership);
        m_quickView->engine()->setObjectOwnership(m_contentItem, QQmlEngine::CppOwnership);
        m_quickView->setContent(m_url, m_component, m_contentItem);

        if(m_contentItem->property("title").isValid()) {
            m_title = m_contentItem->property("title").toString();
            emit titleChanged(m_contentItem->property("title").toString());
        }

        if(m_contentItem->property("favicon").isValid()) {
            m_iconUrl = m_url;
            m_iconUrl.setPath('/' + m_contentItem->property("favicon").toString());
            QNetworkReply* reply = m_quickView->engine()->networkAccessManager()->get(QNetworkRequest(m_iconUrl));
            connect(reply, &QNetworkReply::finished, this, [=]() {
                QIcon icon = QIcon(QPixmap::fromImage(QImage::fromData(reply->readAll())));
                reply->close();
                reply->deleteLater();
                emit this->iconChanged(icon);
            });
        }

        emit loadFinished(true);
    }

    if (!m_component->errors().isEmpty()) {
        foreach (const QQmlError &error, m_component->errors()) {
            m_api->console()->error(error.toString());
        }
    }

}

void QmlView::deepClean(const QObjectList list)
{
    foreach(auto qObject, list) {
        if(qObject->children().count() > 0) {
            deepClean(qObject->children());
        }
        const char *objectName = qObject->metaObject()->className();
        // This two crashes on destroyed if not deleted previosly
        if(strcmp(objectName, "QQuick3DModel") == 0 || strcmp(objectName, "QQuick3DViewport") == 0) {
            delete qObject;
            return;
        }
    }
}


qreal QmlView::zoomFactor()
{
    return m_contentItem->scale();
}


void QmlView::setZoomFactor(const qreal factor)
{
    m_contentItem->setScale(factor);
    m_contentItem->setHeight(m_container->height() * 1.2);
}


void QmlView::toggleDevTools()
{
    if(m_devTools) {
        delete m_devTools;
        m_devTools = nullptr;
    } else {
        m_devTools = new QmlDevTools(m_api->console(), this);
        m_splitter->setStretchFactor(0,3);
        m_splitter->setStretchFactor(1,1);
        m_splitter->addWidget(m_devTools);
    }
}

void QmlView::setUrl(const QUrl &url, const bool reload)
{
    QNetworkRequest req(url);
    if(reload) {
        req.setAttribute(QNetworkRequest::Attribute::CacheLoadControlAttribute, QNetworkRequest::CacheLoadControl::AlwaysNetwork);
    }
    NetworkAccessManagerFactory *nf = static_cast<NetworkAccessManagerFactory*>(m_quickView->engine()->networkAccessManagerFactory());
    nf->setReloading(reload);
    m_reply = mainBrowser.httpManager()->get(req);
    connect(m_reply, &QNetworkReply::finished, this, &QmlView::indexLoaded);
// todo
//    connect(m_reply, &QNetworkReply::downloadProgress, this, [=](qint64 ist, qint64 max) {
//        if(max > 0) {
//            m_loadProgress = 100 / max * ist;
//        }
//    });
}

const QString QmlView::title()
{
    return m_title;
}

const QUrl QmlView::iconUrl()
{
    return m_iconUrl;
}

void QmlView::indexLoaded()
{
    m_url = m_reply->url();
    m_api->setLocationUrl(m_url);

    m_component = new QQmlComponent(m_quickView->engine());
    connect(m_component, &QQmlComponent::statusChanged, this, &QmlView::continueLoad);
    m_component->setData(m_reply->readAll(), m_url);

    if (!m_component->errors().isEmpty()) {
        foreach (const QQmlError &error, m_component->errors()) {
            m_api->console()->error(error.toString());
        }
    }
}

void QmlView::resizeEvent(QResizeEvent *event)
{
    if(m_api) {
        m_api->window()->setHeight(this->height());
        m_api->window()->setWidth(this->width());
    }
}


void QmlView::reload() {
    m_quickView->engine()->clearComponentCache();
    TabView *tabView = dynamic_cast<TabView*>(m_parent);
    tabView->loadUrl(tabView->getCurrentUrl(), true);
}

