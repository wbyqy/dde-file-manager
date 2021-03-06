/*
 * Copyright (C) 2016 ~ 2018 Deepin Technology Co., Ltd.
 *               2016 ~ 2018 dragondjf
 *
 * Author:     dragondjf<dingjiangfeng@deepin.com>
 *
 * Maintainer: dragondjf<dingjiangfeng@deepin.com>
 *             zccrs<zhangjide@deepin.com>
 *             Tangtong<tangtong@deepin.com>
 *
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

#include "frame.h"
#include "constants.h"
#include "wallpaperlist.h"
#include "wallpaperitem.h"
#include "dbus/deepin_wm.h"
#include "thumbnailmanager.h"
#include "appearance_interface.h"
#include "backgroundhelper.h"

#ifndef DISABLE_SCREENSAVER
#include "screensaver_interface.h"
#endif

#include <DButtonBox>
#include <DIconButton>
#include <DWindowManagerHelper>

#include <QApplication>
#include <QDesktopWidget>
#include <QKeyEvent>
#include <QDebug>
#include <QPainter>
#include <QScrollBar>
#include <QScreen>
#include <QVBoxLayout>
#include <QCheckBox>
#include <DApplicationHelper>

#define DESKTOP_BUTTON_ID "desktop"
#define LOCK_SCREEN_BUTTON_ID "lock-screen"
#define SCREENSAVER_BUTTON_ID "screensaver"

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

static bool previewBackground()
{
    if (DWindowManagerHelper::instance()->windowManagerName() == DWindowManagerHelper::DeepinWM)
        return false;

    return DWindowManagerHelper::instance()->windowManagerName() == DWindowManagerHelper::KWinWM
            || !DWindowManagerHelper::instance()->hasBlurWindow();
}

Frame::Frame(Mode mode, QWidget *parent)
    : DBlurEffectWidget(parent)
    , m_mode(mode)
    , m_wallpaperList(new WallpaperList(this))
    , m_closeButton(new DIconButton(this))
    , m_dbusAppearance(new ComDeepinDaemonAppearanceInterface(AppearanceServ,
                                                              AppearancePath,
                                                              QDBusConnection::sessionBus(),
                                                              this))
    , m_mouseArea(new DRegionMonitor(this))
{
    // 截止到dtkwidget 2.0.10版本，在多个屏幕设置不同缩放比时
    // DRegionMonitor 计算的缩放后的坐标可能是错误的
    m_mouseArea->setCoordinateType(DRegionMonitor::Original);

    setFocusPolicy(Qt::StrongFocus);
    setWindowFlags(Qt::BypassWindowManagerHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);

    setBlendMode(DBlurEffectWidget::BehindWindowBlend);

    initUI();
    initSize();

    connect(m_mouseArea, &DRegionMonitor::buttonPress, [this](const QPoint &p, const int button){
        if (button == 4) {
            m_wallpaperList->prevPage();
        } else if (button == 5) {
            m_wallpaperList->nextPage();
        } else {
            qDebug() << "button pressed on blank area, quit.";

            qreal scale = devicePixelRatioF();
            const QRect sRect = this->windowHandle()->screen()->geometry();
            QRect nativeRect = geometry();

            // 获取窗口真实的geometry
            nativeRect.moveTopLeft((nativeRect.topLeft() - sRect.topLeft()) * scale + sRect.topLeft());
            nativeRect.setSize(nativeRect.size() * scale);

            if (!nativeRect.contains(p)) {
                hide();
            }
        }
    });

    m_closeButton->hide();
    connect(m_wallpaperList, &WallpaperList::mouseOverItemChanged,
            this, &Frame::handleNeedCloseButton);
    connect(m_wallpaperList, &WallpaperList::itemPressed,
            this, &Frame::onItemPressed);

    QTimer::singleShot(0, this, &Frame::initListView);
}

Frame::~Frame()
{
    QStringList list = m_needDeleteList;
    QTimer::singleShot(1000, [list](){
        ComDeepinDaemonAppearanceInterface dbusAppearance(AppearanceServ,
                                           AppearancePath,
                                           QDBusConnection::sessionBus(),
                                           nullptr);

        for (const QString &path : list) {
            dbusAppearance.Delete("background", path);
            qDebug() << "delete background" << path;
        }
    });
}

void Frame::show()
{
    if (previewBackground()) {
        if (m_dbusDeepinWM) {
            // 销毁不需要的资源
            m_dbusDeepinWM->deleteLater();
            m_dbusDeepinWM = nullptr;
        }

        if (!m_backgroundHelper) {
            m_backgroundHelper = new BackgroundHelper(true, this);
            // 防止壁纸设置窗口被背景窗口覆盖
            connect(m_backgroundHelper, &BackgroundHelper::backgroundAdded, this, &Frame::activateWindow);
        }
    } else if (!m_dbusDeepinWM) {
        if (m_backgroundHelper) {
            // 销毁不需要的资源
            m_backgroundHelper->deleteLater();
            m_backgroundHelper = nullptr;
        }

        m_dbusDeepinWM = new DeepinWM(DeepinWMServ,
                                      DeepinWMPath,
                                      QDBusConnection::sessionBus(),
                                      this);
    }

    if (m_dbusDeepinWM)
        m_dbusDeepinWM->RequestHideWindows();

    m_mouseArea->registerRegion();

    DBlurEffectWidget::show();
}

void Frame::hide()
{
    emit aboutHide();

    DBlurEffectWidget::hide();
}

void Frame::setMode(Frame::Mode mode)
{
    if (m_mode == mode)
        return;

    if (m_mode == ScreenSaverMode) {
        if (m_backgroundHelper) {
            m_backgroundHelper->setVisible(true);
        }

        m_dbusScreenSaver->Stop();
    }

    m_mode = mode;

    reLayoutTools();
    refreshList();
}

QString Frame::desktopBackground() const
{
    return m_desktopWallpaper;
}

void Frame::handleNeedCloseButton(QString path, QPoint pos)
{
    if (!path.isEmpty()) {
        m_closeButton->adjustSize();
        m_closeButton->move(pos.x() - 10, pos.y() - 10);
        m_closeButton->show();
        m_closeButton->disconnect();

        connect(m_closeButton, &DIconButton::clicked, this, [this, path] {
            m_dbusAppearance->Delete("background", path); // 当前自定义壁纸不一定能删成功
            m_needDeleteList << path;
            m_wallpaperList->removeWallpaper(path);
            m_closeButton->hide();
        }, Qt::UniqueConnection);
    } else {
        m_closeButton->hide();
    }
}

void Frame::showEvent(QShowEvent * event)
{
#ifndef DISABLE_SCREENSAVER
    m_switchModeControl->adjustSize();
#endif

    activateWindow();

    QTimer::singleShot(1, this, &Frame::refreshList);

    DBlurEffectWidget::showEvent(event);
}

void Frame::hideEvent(QHideEvent *event)
{
    DBlurEffectWidget::hideEvent(event);

    if (m_dbusDeepinWM)
        m_dbusDeepinWM->CancelHideWindows();
    m_mouseArea->unregisterRegion();

    if (m_mode == WallpaperMode) {
        if (!m_desktopWallpaper.isEmpty())
            m_dbusAppearance->Set("background", m_desktopWallpaper);
        else if (m_dbusDeepinWM)
            m_dbusDeepinWM->SetTransientBackground("");

        if (!m_lockWallpaper.isEmpty())
            m_dbusAppearance->Set("greeterbackground", m_lockWallpaper);

        ThumbnailManager *manager = ThumbnailManager::instance(devicePixelRatioF());
        manager->stop();
    }
#ifndef DISABLE_SCREENSAVER
    else if (m_mode == ScreenSaverMode) {
        m_dbusScreenSaver->Stop();
    }
#endif

    // 销毁资源
    if (m_dbusDeepinWM) {
        m_dbusDeepinWM->deleteLater();
        m_dbusDeepinWM = nullptr;
    }

    if (m_backgroundHelper) {
        m_backgroundHelper->deleteLater();
        m_backgroundHelper = nullptr;
    }

    emit done();
}

void Frame::keyPressEvent(QKeyEvent * event)
{
    if (event->key() == Qt::Key_Escape) {
        qDebug() << "escape key pressed, quit.";
        hide();
    }

    DBlurEffectWidget::keyPressEvent(event);
}

void Frame::paintEvent(QPaintEvent *event)
{
    DBlurEffectWidget::paintEvent(event);

    QPainter pa(this);

    pa.setCompositionMode(QPainter::CompositionMode_SourceOut);
    pa.setPen(QPen(QColor(255, 255, 255, 20), 1));
    pa.drawLine(QPoint(0, 0), QPoint(width(), 0));
}

bool Frame::event(QEvent *event)
{
#ifndef DISABLE_SCREENSAVER
    if (event->type() == QEvent::LayoutRequest
            || event->type() == QEvent::Resize) {
        adjustModeSwitcherPoint();
    }
#endif

    return DBlurEffectWidget::event(event);
}

#ifndef DISABLE_SCREENSAVER
void Frame::setMode(QAbstractButton * toggledBtn, bool on)
{
    Q_UNUSED(on);

    int mode = m_switchModeControl->buttonList().indexOf(toggledBtn);
    setMode(Mode(mode));
}

void Frame::reLayoutTools()
{
    if (m_mode == ScreenSaverMode) {
        m_waitControlLabel->show();
        m_waitControl->show();
        m_lockScreenBox->show();
#ifndef DISABLE_WALLPAPER_CAROUSEL
        m_wallpaperCarouselCheckBox->hide();
        m_wallpaperCarouselControl->hide();
        layout()->removeItem(m_wallpaperCarouselLayout);
        static_cast<QBoxLayout*>(layout())->insertLayout(0, m_toolLayout);
#endif
    } else {
        m_waitControlLabel->hide();
        m_waitControl->hide();
        m_lockScreenBox->hide();
#ifndef DISABLE_WALLPAPER_CAROUSEL
        m_wallpaperCarouselCheckBox->show();
        m_wallpaperCarouselControl->setVisible(m_wallpaperCarouselCheckBox->isChecked());
        layout()->removeItem(m_toolLayout);
        static_cast<QBoxLayout*>(layout())->insertLayout(0, m_wallpaperCarouselLayout);
#endif
    }
}
#endif

#if !defined(DISABLE_SCREENSAVER) || !defined(DISABLE_WALLPAPER_CAROUSEL)
void Frame::adjustModeSwitcherPoint()
{
    // 调整模式切换控件的位置
    m_switchModeControl->adjustSize();

    // 自己计算宽度，当控件未显示时无法使用layout的sizeHint
    int tools_width = 0;

#ifndef DISABLE_SCREENSAVER
    {
        auto tools_layout_margins = m_toolLayout->contentsMargins();
        int width = m_waitControlLabel->sizeHint().width() +
                    m_waitControl->sizeHint().width() +
                    m_lockScreenBox->sizeHint().width();

        tools_width = tools_layout_margins.left() + width +
                      m_toolLayout->count() * m_toolLayout->spacing();
    }
#endif

#ifndef DISABLE_WALLPAPER_CAROUSEL
    {
        int width = m_wallpaperCarouselCheckBox->sizeHint().width() +
                    m_wallpaperCarouselControl->sizeHint().width() +
                    m_wallpaperCarouselLayout->contentsMargins().left();

        if (width > tools_width) {
            tools_width = width;
        }
    }
#endif

    // 防止在低分辨率情况下切换控件和左边的工具栏重叠
    if (width() / 2 < tools_width) {
        m_switchModeControl->move(width() - m_switchModeControl->width() - 10,
                                  (m_wallpaperList->y() - m_switchModeControl->height()) / 2);
    } else {
        m_switchModeControl->move((width() - m_switchModeControl->width()) / 2,
                                  (m_wallpaperList->y() - m_switchModeControl->height()) / 2);
    }
}
#endif

static QString timeFormat(int second)
{
    quint8 s = second % 60;
    quint8 m = second / 60;
    quint8 h = m / 60;
    quint8 d = h / 24;

    m = m % 60;
    h = h % 24;

    QString time_string;

    if (d > 0) {
        time_string.append(QString::number(d)).append("d");
    }

    if (h > 0) {
        if (!time_string.isEmpty()) {
            time_string.append(' ');
        }

        time_string.append(QString::number(h)).append("h");
    }

    if (m > 0) {
        if (!time_string.isEmpty()) {
            time_string.append(' ');
        }

        time_string.append(QString::number(m)).append("m");
    }

    if (s > 0 || time_string.isEmpty()) {
        if (!time_string.isEmpty()) {
            time_string.append(' ');
        }

        time_string.append(QString::number(s)).append("s");
    }

    return time_string;
}

void Frame::initUI()
{
    m_closeButton->setIcon(QIcon::fromTheme("dfm_close_round_normal"));
    m_closeButton->setFixedSize(24, 24);
    m_closeButton->setIconSize({24, 24});
    m_closeButton->setFlat(true);

    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->setMargin(0);
    layout->setSpacing(0);

    DPalette pal = DApplicationHelper::instance()->palette(this);
    QColor textColor = pal.color(QPalette::Normal, QPalette::BrightText);

#ifndef DISABLE_WALLPAPER_CAROUSEL
    m_wallpaperCarouselLayout = new QHBoxLayout;
    m_wallpaperCarouselCheckBox = new QCheckBox(tr("Wallpaper Slideshow"), this);
    m_wallpaperCarouselCheckBox->setChecked(true);
    QPalette wccPal = m_wallpaperCarouselCheckBox->palette();
    wccPal.setColor(QPalette::All, QPalette::WindowText, textColor);
    m_wallpaperCarouselCheckBox->setPalette(wccPal);
    m_wallpaperCarouselControl = new DButtonBox(this);
    QList<DButtonBoxButton*> wallpaperSlideshowBtns;

    QByteArrayList array_policy {"30", "60", "300", "600", "900", "1800", "3600", "login", "wakeup"};

    {
        int current_policy_index = array_policy.indexOf(m_dbusAppearance->wallpaperSlideShow().toLatin1());

        // 当值不存在此列表时插入此值
        if (current_policy_index < 0) {
            const QString &policy = m_dbusAppearance->wallpaperSlideShow();

            if (!policy.isEmpty()) {
                array_policy.prepend(policy.toLatin1());
            } else {
                m_wallpaperCarouselCheckBox->setChecked(false);
            }

            // wallpaper change default per 10 minutes
            current_policy_index = 3;
        }

        for (const QByteArray &time : array_policy) {
            int index = 0;

            DButtonBoxButton * btn;
            if (time == "login") {
                btn = new DButtonBoxButton(tr("When login"), this);
            } else if (time == "wakeup") {
                btn = new DButtonBoxButton(tr("When wakeup"), this);
            } else {
                bool ok = false;
                int t = time.toInt(&ok);
                btn = new DButtonBoxButton(ok ? timeFormat(t) : time, this);
            }

            btn->setMinimumWidth(40);
            wallpaperSlideshowBtns.append(btn);
        }

        m_wallpaperCarouselControl->setButtonList(wallpaperSlideshowBtns, true);
        wallpaperSlideshowBtns[current_policy_index]->setChecked(true);
        m_wallpaperCarouselControl->setVisible(m_wallpaperCarouselCheckBox->isChecked());
    }

    m_wallpaperCarouselLayout->setSpacing(10);
    m_wallpaperCarouselLayout->setContentsMargins(20, 5, 20, 5);
    m_wallpaperCarouselLayout->addWidget(m_wallpaperCarouselCheckBox);
    m_wallpaperCarouselLayout->addWidget(m_wallpaperCarouselControl);
    m_wallpaperCarouselLayout->addItem(new QSpacerItem(1, HeaderSwitcherHeight));
    m_wallpaperCarouselLayout->addStretch();

    layout->addLayout(m_wallpaperCarouselLayout);

    connect(m_wallpaperCarouselCheckBox, &QCheckBox::clicked, this, [this, array_policy] (bool checked) {
        m_wallpaperCarouselControl->setVisible(checked);

        int checkedIndex = m_wallpaperCarouselControl->buttonList().indexOf(m_wallpaperCarouselControl->checkedButton());
        if (!checked) {
            m_dbusAppearance->setWallpaperSlideShow(QString());
        } else if (checkedIndex >= 0) {
            m_dbusAppearance->setWallpaperSlideShow(array_policy.at(checkedIndex));
        }
    });
    connect(m_wallpaperCarouselControl, &DButtonBox::buttonToggled, this, [this, array_policy] (QAbstractButton * toggledBtn, bool) {
        m_dbusAppearance->setWallpaperSlideShow(array_policy.at(m_wallpaperCarouselControl->buttonList().indexOf(toggledBtn)));
    });
#endif

#ifndef DISABLE_SCREENSAVER
    m_toolLayout = new QHBoxLayout;

    m_waitControl = new DButtonBox(this);
    m_lockScreenBox = new QCheckBox(tr("Require a password on wakeup"), this);
    QPalette lsPal = m_lockScreenBox->palette();
    lsPal.setColor(QPalette::All, QPalette::WindowText, textColor);
    m_lockScreenBox->setPalette(lsPal);

    QVector<int> time_array {60, 300, 600, 900, 1800, 3600, 0};
    QList<DButtonBoxButton*> timeArrayBtns;

    if (!m_dbusScreenSaver) {
        m_dbusScreenSaver = new ComDeepinScreenSaverInterface("com.deepin.ScreenSaver", "/com/deepin/ScreenSaver",
                                                              QDBusConnection::sessionBus(), this);
    }

    int current_wait_time_index = time_array.indexOf(m_dbusScreenSaver->linePowerScreenSaverTimeout());

    // 当值不存在此列表时插入此值
    if (current_wait_time_index < 0) {
        int timeout = m_dbusScreenSaver->linePowerScreenSaverTimeout();
        time_array.prepend(timeout);
        current_wait_time_index = 0;
    }

    for (const int time : time_array) {
        if (time > 0) {
            DButtonBoxButton * btn = new DButtonBoxButton(timeFormat(time), this);
            btn->setMinimumWidth(40);
            timeArrayBtns.append(btn);
        }
    }

    timeArrayBtns.append(new DButtonBoxButton(tr("Never"), this));
    m_waitControlLabel = new QLabel(tr("Wait:"), this);
    QPalette wcPal = m_waitControlLabel->palette();
    wcPal.setColor(QPalette::All, QPalette::WindowText, textColor);
    m_waitControlLabel->setPalette(wcPal);
    m_waitControl->setButtonList(timeArrayBtns, true);
    timeArrayBtns[current_wait_time_index]->setChecked(true);
    m_lockScreenBox->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_lockScreenBox->setChecked(m_dbusScreenSaver->lockScreenAtAwake());

    m_toolLayout->setSpacing(10);
    m_toolLayout->setContentsMargins(20, 10, 20, 10);
    m_toolLayout->addWidget(m_waitControlLabel);
    m_toolLayout->addWidget(m_waitControl);
    m_toolLayout->addSpacing(10);
    m_toolLayout->addWidget(m_lockScreenBox, 1, Qt::AlignLeft);

#ifdef DISABLE_WALLPAPER_CAROUSEL
    // 在布局中占位，保证布局的高度
    QWidget *fake_layout = new QWidget(this);

    fake_layout->setFixedHeight(m_waitControl->height());
    fake_layout->setWindowFlags(Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    fake_layout->lower();
    m_toolLayout->addWidget(fake_layout);
#endif

    layout->addLayout(m_toolLayout);
    layout->addWidget(m_wallpaperList);
    layout->addSpacing(10);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    //###(zccrs): 直接把switModeControl放到布局中始终无法在两种mos模式下都居中
    // 使用anchors使此控件居中
    DButtonBoxButton * wallpaperBtn = new DButtonBoxButton(tr("Wallpaper"), this);
    DButtonBoxButton * screensaverBtn = new DButtonBoxButton(tr("Screensaver"), this);
    wallpaperBtn->setMinimumWidth(40);
    screensaverBtn->setMinimumWidth(40);
    m_switchModeControl = new DButtonBox(this);
    m_switchModeControl->setButtonList({wallpaperBtn, screensaverBtn}, true);
    if (m_mode == WallpaperMode) {
        wallpaperBtn->setChecked(true);
    } else {
        screensaverBtn->setChecked(true);
    }

    connect(m_waitControl, &DButtonBox::buttonToggled, this, [this, time_array] (QAbstractButton * toggleBtn, bool) {
        int index = m_waitControl->buttonList().indexOf(toggleBtn);
        m_dbusScreenSaver->setBatteryScreenSaverTimeout(time_array[index]);
        m_dbusScreenSaver->setLinePowerScreenSaverTimeout(time_array[index]);
    });

    connect(m_switchModeControl, &DButtonBox::buttonToggled, this, static_cast<void(Frame::*)(QAbstractButton*, bool)>(&Frame::setMode));
    connect(m_lockScreenBox, &QCheckBox::toggled, m_dbusScreenSaver, &ComDeepinScreenSaverInterface::setLockScreenAtAwake);

    reLayoutTools();
#elif !defined(DISABLE_WALLPAPER_CAROUSEL)
    layout->addWidget(m_wallpaperList);
#endif

    layout->addStretch();
}

void Frame::initSize()
{
    const QRect primaryRect = qApp->primaryScreen()->geometry();
    int actualHeight;
#if defined(DISABLE_SCREENSAVER) && defined(DISABLE_WALLPAPER_CAROUSEL)
    actualHeight = FrameHeight;
#else
    actualHeight = FrameHeight + HeaderSwitcherHeight;
#endif
    setFixedSize(primaryRect.width()-20, actualHeight);

    qDebug() << "move befor: " << this->geometry() << m_wallpaperList->geometry();
    move(primaryRect.x()+10, primaryRect.y() + primaryRect.height() - height());
    qDebug() << "this move : " << this->geometry() << m_wallpaperList->geometry();
    m_wallpaperList->setFixedSize(primaryRect.width()-20, ListHeight);
}

void Frame::initListView()
{

}

void Frame::refreshList()
{
    m_wallpaperList->hide();
    m_wallpaperList->clear();
    m_wallpaperList->show();

    if (m_mode == WallpaperMode) {
        QDBusPendingCall call = m_dbusAppearance->List("background");
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, call] {
            if (call.isError()) {
                qWarning() << "failed to get all backgrounds: " << call.error().message();
            } else {
                QDBusReply<QString> reply = call.reply();
                QString value = reply.value();
                QStringList strings = processListReply(value);
                foreach (QString path, strings) {
                    if (m_needDeleteList.contains(QUrl(path).path())) {
                        continue;
                    }
                    WallpaperItem * item = m_wallpaperList->addWallpaper(path);
                    item->setData(item->getPath());
                    item->setDeletable(m_deletableInfo.value(path));
                    item->addButton(DESKTOP_BUTTON_ID, tr("Only desktop"));
                    item->addButton(LOCK_SCREEN_BUTTON_ID, tr("Only lock screen"));
                    item->show();

                    connect(item, &WallpaperItem::buttonClicked, this, &Frame::onItemButtonClicked);
                }

                m_wallpaperList->setFixedWidth(width());
                m_wallpaperList->updateItemThumb();
                m_wallpaperList->show();
            }
        });
    }
#ifndef DISABLE_SCREENSAVER
    else if (m_mode == ScreenSaverMode) {
        if (!m_dbusScreenSaver) {
            m_dbusScreenSaver = new ComDeepinScreenSaverInterface("com.deepin.ScreenSaver", "/com/deepin/ScreenSaver",
                                                                  QDBusConnection::sessionBus(), this);
        }

        const QStringList &saver_name_list = m_dbusScreenSaver->allScreenSaver();

        for (const QString &name : saver_name_list) {
            const QString &cover_path = m_dbusScreenSaver->GetScreenSaverCover(name);

            WallpaperItem *item = m_wallpaperList->addWallpaper(cover_path);
            item->setData(name);
            item->setUseThumbnailManager(false);
            item->setDeletable(false);
            item->addButton(SCREENSAVER_BUTTON_ID, tr("Apply"));
            item->show();

            connect(item, &WallpaperItem::buttonClicked, this, &Frame::onItemButtonClicked);
        }

        m_wallpaperList->setFixedWidth(width());
        m_wallpaperList->updateItemThumb();
        m_wallpaperList->show();
    }
#endif
}

void Frame::onItemPressed(const QString &data)
{
    if (m_mode == WallpaperMode) {
        if (m_dbusDeepinWM)
            m_dbusDeepinWM->SetTransientBackground(data);

        if (m_backgroundHelper)
            m_backgroundHelper->setBackground(data);

        m_desktopWallpaper = data;
        m_lockWallpaper = data;

        // 点击当前壁纸不显示删除按钮
        if (m_closeButton && m_closeButton->isVisible()) {
            m_closeButton->hide();
        }

        {
            for (int i=0;i<m_wallpaperList->count();++i) {
                WallpaperItem *item = dynamic_cast<WallpaperItem *>(m_wallpaperList->item(i));
                if (item) {
                    bool isCustom = item->data().contains("custom-wallpapers");
                    if (!isCustom) {
                        continue;
                    }
                    bool isCurrent = m_backgroundHelper?(item->data()==m_backgroundHelper->background()):false;
                    bool isDeletable = item->getDeletable();
                    item->setDeletable(!isCurrent && (isDeletable || isCustom));
                }
            }
        }
    }
#ifndef DISABLE_SCREENSAVER
    else if (m_mode == ScreenSaverMode) {
        // 防止壁纸背景盖住屏保预览窗口
        if (m_backgroundHelper) {
            m_backgroundHelper->setVisible(false);
        }

        m_dbusScreenSaver->Preview(data, 1);
    }
#endif
}

void Frame::onItemButtonClicked(const QString &buttonID)
{
    WallpaperItem *item = qobject_cast<WallpaperItem*>(sender());

    if (!item)
        return;

    if (buttonID == DESKTOP_BUTTON_ID) {
        m_lockWallpaper.clear();
    } else if (buttonID == LOCK_SCREEN_BUTTON_ID) {
        m_desktopWallpaper.clear();
    }
#ifndef DISABLE_SCREENSAVER
    else if (buttonID == SCREENSAVER_BUTTON_ID) {
        m_dbusScreenSaver->setCurrentScreenSaver(item->data());
    }
#endif

    hide();
}

QStringList Frame::processListReply(const QString &reply)
{
    QStringList result;

    QJsonDocument doc = QJsonDocument::fromJson(reply.toUtf8());
    if (doc.isArray()) {
        QJsonArray arr = doc.array();
        foreach (QJsonValue val, arr) {
            QJsonObject obj = val.toObject();
            QString id = obj["Id"].toString();
            result.append(id);
            m_deletableInfo[id] = obj["Deletable"].toBool();
        }
    }

    return result;
}
