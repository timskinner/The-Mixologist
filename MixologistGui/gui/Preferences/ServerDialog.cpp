/****************************************************************
 *  Copyright 2010, Fair Use, Inc.
 *  Copyright 2006, crypton
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


#include "ServerDialog.h"
#include "gui/MainWindow.h" //for settings files
#include <QSettings>
#include <iostream>
#include <sstream>

#include "interface/iface.h"
#include "interface/peers.h"

#include <QTimer>

/** Constructor */
ServerDialog::ServerDialog(QWidget *parent)
    : ConfigPage(parent) {
    /* Invoke the Qt Designer generated object setup routine */
    ui.setupUi(this);

    connect(ui.mixologyServer, SIGNAL(editingFinished()), this, SLOT(editedServer()));
    connect(ui.randomizePorts, SIGNAL(toggled(bool)), this, SLOT(randomizeToggled(bool)));

    /* Setup display */
    QSettings settings(*mainSettings, QSettings::IniFormat, this);
    ui.UPNP->setChecked(settings.value("Network/UPNP", DEFAULT_UPNP).toBool());
    ui.randomizePorts->setChecked(settings.value("Network/PortNumber", DEFAULT_PORT).toInt() == SET_TO_RANDOMIZED_PORT);

    /* Load up actual port number  */
    PeerDetails detail;
    if (peers->getPeerDetails(peers->getOwnLibraryMixerId(), detail)) {
        ui.portNumber->setValue(detail.localPort);
    }
    ui.portNumber->setMinimum(Peers::MIN_PORT);
    ui.portNumber->setMaximum(Peers::MAX_PORT);
    ui.portNumber->setEnabled(settings.value("Network/PortNumber", DEFAULT_PORT).toInt() != SET_TO_RANDOMIZED_PORT);

    QSettings serverSettings(*startupSettings, QSettings::IniFormat, this);
    ui.mixologyServer->setText(serverSettings.value("MixologyServer", DEFAULT_MIXOLOGY_SERVER).toString());
}

/** Saves the changes on this page */
bool ServerDialog::save() {
    QSettings settings(*mainSettings, QSettings::IniFormat, this);
    settings.setValue("Network/UPNP", ui.UPNP->isChecked());
    if (ui.randomizePorts->isChecked()) settings.setValue("Network/PortNumber", -1);
    else settings.setValue("Network/PortNumber", ui.portNumber->value());
    QSettings serverSettings(*startupSettings, QSettings::IniFormat, this);
    serverSettings.setValue("MixologyServer", ui.mixologyServer->text());

    return true;
}

void ServerDialog::editedServer(){
    if (ui.mixologyServer->text().isEmpty()){
        ui.mixologyServer->setText(DEFAULT_MIXOLOGY_SERVER);
    }
}

void ServerDialog::randomizeToggled(bool set){
    ui.portNumber->setEnabled(!set);
}
