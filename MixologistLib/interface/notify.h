/****************************************************************
 *  Copyright 2010, Fair Use, Inc.
 *  Copyright 2007-8, Robert Fernie.
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

#ifndef NOTIFY_GUI_INTERFACE_H
#define NOTIFY_GUI_INTERFACE_H

#include <string>
#include <stdint.h>
#include <QString>

class Notify;
extern Notify   *notify;

const uint32_t SYS_ERROR    = 0x0001;
const uint32_t SYS_WARNING  = 0x0002;
const uint32_t SYS_INFO     = 0x0004;

//Indicates a new connection to a friend.
const uint32_t POPUP_CONNECT   = 0x0001;
//Indicates a completed download.
const uint32_t POPUP_DOWNDONE  = 0x0002;
//Indicates an unmatched file was encountered.
const uint32_t POPUP_UNMATCHED = 0x0004;

/* This class is the output interface for notification events. */

class Notify {
public:

    Notify() {
        return;
    }
    virtual ~Notify() {
        return;
    }

    /* These methods return an unread message and removes it from the queue. */
    virtual bool NotifySysMessage(uint32_t &sysid, uint32_t &type,
                                  QString &title, QString &msg)     = 0;
    virtual bool NotifyPopupMessage(uint32_t &ptype, QString &name, QString &msg)   = 0;

};


#endif
