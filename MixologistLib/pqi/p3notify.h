#ifndef P3_NOTIFY_INTERFACE_H
#define P3_NOTIFY_INTERFACE_H

/****************************************************************
 *  Copyright 2010, Fair Use, Inc.
 *  Copyright 2007-8, Robert Fernie
 *
 *  This file is part of the Mixologist.
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

#include "interface/notify.h"
#include "pqi/pqinotify.h"

#include "util/threads.h"
#include <list>

class p3NotifySysMsg {
public:

    uint32_t sysid;
    uint32_t type;
    QString title;
    QString msg;
};

class p3NotifyPopupMsg {
public:

    uint32_t type;
    QString name;
    QString msg;
};

/* This class is the integrated implementation of pqinotify (input) and notify (output) for notification events. */

class p3Notify: public Notify, public pqiNotify {
public:

    p3Notify() {
        return;
    }
    virtual ~p3Notify() {
        return;
    }

    /* Output for gui */
    //These pop off an element from the pending messages lists and uses it to populate the values.
    virtual bool NotifySysMessage(uint32_t &sysid, uint32_t &type,
                                  QString &title, QString &msg);
    virtual bool NotifyPopupMessage(uint32_t &ptype, QString &name, QString &msg);

    /* Overloaded from pqiNotify */
    //These methods add messages to the pending messages lists.
    virtual bool AddPopupMessage(uint32_t ptype, QString name, QString msg);
    virtual bool AddSysMessage(uint32_t sysid, uint32_t type, QString title, QString msg);

private:

    MixMutex noteMtx;

    //These store the pending messages.
    std::list<p3NotifySysMsg> pendingSysMsgs;
    std::list<p3NotifyPopupMsg> pendingPopupMsgs;
};


#endif
