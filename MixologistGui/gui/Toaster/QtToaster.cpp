/****************************************************************
 *  Copyright 2010, Fair Use, Inc.
 *  Copyright 2006-2007, crypton
 *
 *  This file is part of The Mixologist.
 *
 *  The Mixologist is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  The Mixologist is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with the Mixologist; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301, USA.
 ****************************************************************/

#include "QtToaster.h"

#include <QtGui/QtGui>

static const unsigned TIME_TO_SHOW = 20;

QtToaster::QtToaster(QWidget *toaster)
    : QObject(toaster) {

    _timer = NULL;
    _show = true;

    _toaster = toaster;
    _toaster->setParent(_toaster->parentWidget(), Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    //  _toaster->resize(184, 128);
}

void QtToaster::setTimeOnTop(unsigned time) {
    _timeOnTop = time;
}

void QtToaster::close() {
    if (_timer) {
        _timer->stop();
    }
    _toaster->close();
}

void QtToaster::show() {
    //10 pixels of margin

    QDesktopWidget *desktop = QApplication::desktop();
    QRect screenGeometry = desktop->screenGeometry(desktop->primaryScreen());

    _toaster->move(screenGeometry.right() - _toaster->size().width(), screenGeometry.bottom());

    _toaster->show();

    _timer = new QTimer(this);
    connect(_timer, SIGNAL(timeout()), SLOT(changeToasterPosition()));
    _timer->start(TIME_TO_SHOW);
}

void QtToaster::changeToasterPosition() {
    QDesktopWidget *desktop = QApplication::desktop();
    QPoint p = _toaster->pos();

    //Toaster is showing slowly
    if (_show) {
        _toaster->move(p.x(), p.y() - 3);

        QRect desktopGeometry = desktop->availableGeometry(desktop->primaryScreen());

        if (p.y() < (desktopGeometry.bottom() - _toaster->size().height() - 5)) {
            //Toaster should be hidden now
            _show = false;
            _timer->stop();
            //Waits 5 seconds with the toaster on top
            _timer->start(_timeOnTop);
        }
    }

    //Toaster is hiding slowly
    else {
        _toaster->move(p.x(), p.y() + 3);

        QRect screenGeometry = desktop->screenGeometry(desktop->primaryScreen());

        _timer->stop();
        _timer->start(TIME_TO_SHOW);

        if (p.y() > (screenGeometry.bottom())) {
            //Closes the toaster -> hide it completely
            close();
        }
    }
}
