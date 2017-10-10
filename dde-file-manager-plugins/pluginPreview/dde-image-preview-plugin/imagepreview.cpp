/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "imagepreview.h"

#include <QImageReader>
#include <QProcess>
#include <QMimeDatabase>
#include <QMimeType>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileInfo>

#include <anchors.h>

#include "imageview.h"

DWIDGET_USE_NAMESPACE

DFM_BEGIN_NAMESPACE

ImagePreview::ImagePreview(QObject *parent)
    : DFMFilePreview(parent)
{
}

ImagePreview::~ImagePreview()
{
    if (m_imageView)
        m_imageView->deleteLater();

    if (m_messageStatusBar)
        m_messageStatusBar->deleteLater();
}

bool ImagePreview::canPreview(const QUrl &url) const
{
    const QByteArray &format = QImageReader::imageFormat(url.toLocalFile());

    return QImageReader::supportedImageFormats().contains(format);
}

void ImagePreview::initialize(QWidget *window, QWidget *statusBar)
{
    Q_UNUSED(window)

    m_messageStatusBar = new QLabel(statusBar);
    m_messageStatusBar->setStyleSheet("QLabel{font-family: Helvetica;\
                                   font-size: 12px;\
                                   font-weight: 300;\
                                   color: #545454;}");

    AnchorsBase(m_messageStatusBar).setCenterIn(statusBar);
}

bool ImagePreview::setFileUrl(const DUrl &url)
{
    if (m_url == url)
        return true;

    if (!url.isLocalFile())
        return false;

    if (!canPreview(url))
        return false;

    m_url = url;

    if (!m_imageView)
        m_imageView = new ImageView(url.toLocalFile());
    else
        m_imageView->setFile(url.toLocalFile());

    const QSize &image_size = m_imageView->sourceSize();

    m_messageStatusBar->setText(QString("%1x%2").arg(image_size.width()).arg(image_size.height()));
    m_messageStatusBar->adjustSize();

    m_title = QFileInfo(url.toLocalFile()).fileName();

    Q_EMIT titleChanged();

    return true;
}

QWidget *ImagePreview::contentWidget() const
{
    return m_imageView;
}

QString ImagePreview::title() const
{
    return m_title;
}

DFM_END_NAMESPACE