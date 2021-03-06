/****************************************************************
 *  Copyright 2010, Fair Use, Inc.
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

#include "ft/ftfilewatcher.h"
#include <server/librarymixer-library.h>
#include <services/mixologyservice.h>
#include <interface/iface.h> //For the libraryconnect and notifyBase
#include <interface/librarymixer-connect.h>
#include <interface/init.h>
#include <interface/peers.h> //For the peers variable
#include <ft/ftserver.h>
#include <util/dir.h>
#include <util/xml.h>
#include <pqi/pqinotify.h>
#include <QFile>
#include <QDomDocument>

#define LIBRARYFILE "library.xml"

LibraryMixerLibraryManager::LibraryMixerLibraryManager() {
    connect(fileWatcher, SIGNAL(newFileHash(QString,qlonglong,uint,QString)), this, SLOT(newFileHash(QString,qlonglong,uint,QString)), Qt::QueuedConnection);
    connect(fileWatcher, SIGNAL(oldHashInvalidated(QString,qlonglong,uint)), this, SLOT(oldHashInvalidated(QString,qlonglong,uint)), Qt::QueuedConnection);
    connect(fileWatcher, SIGNAL(fileRemoved(QString)), this, SLOT(fileRemoved(QString)));

    QDomElement rootNode;
    if (XmlUtil::openXml(LIBRARYFILE, xml, rootNode, "library", QIODevice::ReadWrite)) {
        QDomNode currentNode = rootNode.firstChildElement("item");
        while (!currentNode.isNull()) {
            LibraryMixerLibraryItem* currentItem = new LibraryMixerLibraryItem(currentNode);
            connect(currentItem, SIGNAL(fileNoLongerAvailable(QString,qulonglong)), this, SIGNAL(fileNoLongerAvailable(QString,qulonglong)));

            libraryList.insert(currentItem->id(), currentItem);
            emit libraryItemInserted(currentItem->id());

            currentNode = currentNode.nextSiblingElement("item");
        }
    }
    /* No need to save XML here. Even if we just created a new XML, no data has been inserted yet so nothing worth saving. */
}

void LibraryMixerLibraryManager::mergeLibrary(const QDomElement &libraryElement) {
    QMutexLocker stack(&libMutex);

    QDomElement currentItemElement = libraryElement.firstChildElement("item");
    QList<unsigned int> preexistingItems = libraryList.keys();

    /* Add or update all items in the new list. */
    while (!currentItemElement.isNull()) {
        unsigned int itemId = currentItemElement.firstChildElement("id").text().toUInt();
        if (libraryList.contains(itemId)) {
            preexistingItems.removeOne(itemId);
            if (libraryList[itemId]->title() != currentItemElement.firstChildElement("title").text()) {
                libraryList[itemId]->title(currentItemElement.firstChildElement("title").text());
                emit libraryStateChanged(itemId);
            }
        } else {
            QDomElement newItemElement = currentItemElement.cloneNode(true).toElement();
            xml.documentElement().appendChild(newItemElement);
            newItemElement.appendChild(XmlUtil::createElement(xml, "itemstate", QString::number(LibraryMixerItem::UNMATCHED)));
            newItemElement.appendChild(XmlUtil::createElement(xml, "autoresponse"));

            LibraryMixerLibraryItem* newItem = new LibraryMixerLibraryItem(newItemElement);
            connect(newItem, SIGNAL(fileNoLongerAvailable(QString,qulonglong)), this, SIGNAL(fileNoLongerAvailable(QString,qulonglong)));

            libraryList.insert(newItem->id(), newItem);
            emit libraryItemInserted(newItem->id());
        }
        currentItemElement = currentItemElement.nextSiblingElement("item");
    }

    /* Delete any items removed online. */
    while (!preexistingItems.isEmpty()) {
        unsigned int itemId = preexistingItems.first();
        preexistingItems.removeFirst();
        delete libraryList[itemId];
        libraryList.remove(itemId);
        emit libraryItemRemoved(itemId);
    }

    XmlUtil::writeXml(LIBRARYFILE, xml);

    /* Check if any items are missing their hashes, continue hashing if so.
       fileCount() returns 0 if there are no files because the item state has no associated files. */
    foreach (LibraryMixerLibraryItem* currentItem, libraryList.values()) {
        for (int i = 0; i < currentItem->fileCount(); i++) {
            if (currentItem->hashes()[i].isEmpty()) fileWatcher->addFile(currentItem->paths()[i]);
            else {
                fileWatcher->addFile(currentItem->paths()[i], currentItem->filesizes()[i], currentItem->modified()[i]);
            }
        }
    }
}

void LibraryMixerLibraryManager::getLibrary(QMap<unsigned int, LibraryMixerItem> &library) const {
    QMutexLocker stack(&libMutex);
    foreach (LibraryMixerLibraryItem* currentItem, libraryList.values()) {
        library[currentItem->id()] = internalConvertItem(currentItem);
    }
}

bool LibraryMixerLibraryManager::setMatchChat(unsigned int item_id) {
    QMutexLocker stack(&libMutex);

    LibraryMixerLibraryItem* item = libraryList.value(item_id);
    if (item == NULL) return false;

    if (item->itemState(LibraryMixerItem::LibraryMixerItem::MATCHED_TO_CHAT)) {
        emit libraryStateChanged(item_id);
        XmlUtil::writeXml(LIBRARYFILE, xml);
        return true;
    }
    return false;
}

bool LibraryMixerLibraryManager::setMatchFile(unsigned int item_id, QStringList paths, LibraryMixerItem::ItemState itemstate, unsigned int recipient) {
    QMutexLocker stack(&libMutex);

    if (itemstate != LibraryMixerItem::MATCHED_TO_FILE &&
        itemstate != LibraryMixerItem::MATCHED_TO_LEND) return false;

    LibraryMixerLibraryItem* item = libraryList.value(item_id);
    if (item == NULL) return false;

    item->itemState(itemstate);

    if (!paths.empty()){
        item->paths(paths);
        foreach(QString path, paths){
            fileWatcher->addFile(path);
        }
    }

    emit libraryStateChanged(item_id);

    XmlUtil::writeXml(LIBRARYFILE, xml);

    if (recipient != 0) item->sendToOnHashList.append(recipient);

    return true;
}

bool LibraryMixerLibraryManager::setMatchMessage(unsigned int item_id, QString message) {
    QMutexLocker stack(&libMutex);

    LibraryMixerLibraryItem* item = libraryList.value(item_id);
    if (item == NULL) return false;

    if (item->message(message)) {
        emit libraryStateChanged(item_id);
        XmlUtil::writeXml(LIBRARYFILE, xml);
        return true;
    }
    return false;
}

bool LibraryMixerLibraryManager::setLent(unsigned int friend_id, const QString &item_id) {
    unsigned int numeric_item_id = item_id.toUInt();

    QMutexLocker stack(&libMutex);

    LibraryMixerLibraryItem* item = libraryList.value(numeric_item_id);
    if (item == NULL) return false;

    if (item->itemState() == LibraryMixerItem::MATCHED_TO_LEND) {
        if (item->lentTo(friend_id)) {
            emit libraryStateChanged(numeric_item_id);
            if (XmlUtil::writeXml(LIBRARYFILE, xml)) {
                for (int i = 0; i < item->fileCount(); i++) {
                    fileWatcher->stopWatching(item->paths()[i]);
                    if (!QFile::remove(item->paths()[i]))
                        getPqiNotify()->AddSysMessage(SYS_WARNING, "File remove error", "Unable to remove borrowed file " + item->paths()[i]);
                }
            }
            return true;
        }
    }
    return false;
}

int LibraryMixerLibraryManager::returnBorrowedFiles(unsigned int friend_id, const QString &item_id, const QStringList &paths, const QStringList &hashes,
                                                    const QList<qlonglong> &filesizes, const QList<bool> &moveFile, QString &finalDestination) {
    /* Find and update the item in question. */
    LibraryMixerLibraryItem* item;
    {
        QMutexLocker stack(&libMutex);
        unsigned int numeric_item_id = item_id.toInt();

        if (!libraryList.contains(numeric_item_id)) return -3;
        if (libraryList.value(numeric_item_id)->itemState() != LibraryMixerItem::MATCHED_TO_LENT) return -4;
        if (libraryList.value(numeric_item_id)->lentTo() != friend_id) return -5;

        item = libraryList.value(numeric_item_id);
        item->itemState(LibraryMixerItem::MATCHED_TO_LEND);
        XmlUtil::writeXml(LIBRARYFILE, xml);
    }

    int result = DirUtil::returnFilesToOriginalLocations(paths, hashes, filesizes, moveFile, item->paths(), item->hashes(), item->filesizes());
    if (result == 1) finalDestination = DirUtil::findCommonParent(item->paths());

    for (int i = 0; i < item->fileCount(); i++) {
        fileWatcher->addFile(item->paths()[i], item->filesizes()[i], item->modified()[i]);
    }

    return result;
}

bool LibraryMixerLibraryManager::getLibraryMixerItem(unsigned int id, LibraryMixerItem &itemToFill) const {
    QMutexLocker stack(&libMutex);

    if (!libraryList.contains(id)) return false;

    itemToFill = internalConvertItem(libraryList[id]);
    return true;
}

bool LibraryMixerLibraryManager::getLibraryMixerItem(QStringList paths, LibraryMixerItem &itemToFill) const {
    QMutexLocker stack(&libMutex);

    foreach(LibraryMixerLibraryItem* item, libraryList.values()){
        if (DirUtil::allPathsMatch(item->paths(), paths)) {
            itemToFill = internalConvertItem(item);
            return true;
        }
    }
    return false;
}

int LibraryMixerLibraryManager::getLibraryMixerItemWithCheck(unsigned int item_id, LibraryMixerItem &item) {
    QMutexLocker stack(&libMutex);

    if (!libraryList.contains(item_id)) return -1;
    if (!libraryList[item_id]->fullyHashed()) return 0;

    item = internalConvertItem(libraryList[item_id]);
    return 1;
}

void LibraryMixerLibraryManager::MixologySuggest(unsigned int friend_id, int item_id) {
    QMutexLocker stack(&libMutex);

    if (libraryList.contains(item_id)) {
        if (libraryList[item_id]->fullyHashed()) {
            int flags;
            if (libraryList[item_id]->itemState() == LibraryMixerItem::MATCHED_TO_LEND) flags = MixologySuggestion::OFFER_LEND;
            else flags = MixologySuggestion::OFFER_SEND;
            mixologyService->sendSuggestion(friend_id,
                                            libraryList[item_id]->title(),
                                            DirUtil::getRelativePaths(libraryList[item_id]->paths()),
                                            libraryList[item_id]->hashes(),
                                            libraryList[item_id]->filesizes());

        } else {
            if (!libraryList[item_id]->sendToOnHashList.contains(friend_id)) {
                libraryList[item_id]->sendToOnHashList.append(friend_id);
            }
            notifyBase->notifyUserOptional(friend_id, NotifyBase::NOTIFY_USER_SUGGEST_WAITING, libraryList[item_id]->title());
        }
    }
}

ftFileMethod::searchResult LibraryMixerLibraryManager::search(const QString &hash, qlonglong size, uint32_t hintflags, unsigned int /*librarymixer_id*/, QString &path) {
    if (hintflags | FILE_HINTS_ITEM) {
        QMutexLocker stack(&libMutex);
        foreach(LibraryMixerLibraryItem* item, libraryList.values()){
            if (item->itemState() == LibraryMixerItem::MATCHED_TO_FILE ||
                item->itemState() == LibraryMixerItem::MATCHED_TO_LEND) {
                for (int i = 0; i < item->fileCount(); i++) {
                    if (hash == item->hashes()[i] &&
                        size == item->filesizes()[i]) {
                        path = item->paths()[i];
                        return ftFileMethod::SEARCH_RESULT_FOUND_SHARED_FILE;
                    }
                }
            }
        }
    }
    return ftFileMethod::SEARCH_RESULT_NOT_FOUND;
}

void LibraryMixerLibraryManager::oldHashInvalidated(QString path, qlonglong size, unsigned int modified) {
    newFileHash(path, size, modified, "");
}

void LibraryMixerLibraryManager::newFileHash(QString path, qlonglong size, unsigned int modified, QString newHash) {
    QMutexLocker stack(&libMutex);

    bool toSave = false;
    foreach(unsigned int item_id, libraryList.keys()) {
        /* This is matching to all file states, except for MATCHED_TO_LENT. If a file has been lent out, we don't want
           to change anything about our information about its files. */
        if (libraryList[item_id]->itemState() == LibraryMixerItem::MATCHED_TO_FILE ||
            libraryList[item_id]->itemState() == LibraryMixerItem::MATCHED_TO_LEND ||
            libraryList[item_id]->itemState() == LibraryMixerItem::MATCH_NOT_FOUND) {

            for (int i = 0; i < libraryList[item_id]->fileCount(); i++) {
                if (path == libraryList[item_id]->paths()[i]) {
                    libraryList[item_id]->setFileInfo(path, size, modified, newHash);
                    toSave = true;
                }
            }

            /* If this was the last hash we needed to complete our collection of hashes, then:
               -if we were match not found, we are now fixed
               -this is the time when we should send to all friends who've been waiting for this. */
            if (libraryList[item_id]->fullyHashed()) {
                if (libraryList[item_id]->itemState() == LibraryMixerItem::MATCH_NOT_FOUND) {
                    libraryList[item_id]->itemState(LibraryMixerItem::MATCHED_TO_FILE);
                    emit libraryStateChanged(item_id);
                }

                foreach (unsigned int friend_id, libraryList[item_id]->sendToOnHashList) {
                    mixologyService->sendSuggestion(friend_id,
                                                    libraryList[item_id]->title(),
                                                    DirUtil::getRelativePaths(libraryList[item_id]->paths()),
                                                    libraryList[item_id]->hashes(),
                                                    libraryList[item_id]->filesizes());
                }
                libraryList[item_id]->sendToOnHashList.clear();
            }
        }
    }
    if (toSave) XmlUtil::writeXml(LIBRARYFILE, xml);
}

void LibraryMixerLibraryManager::fileRemoved(QString path) {
    QMutexLocker stack(&libMutex);

    bool toSave = false;
    foreach(unsigned int item_id, libraryList.keys()) {
        /* This is matching to all file states, except for MATCHED_TO_LENT. If a file has been lent out, we don't want
           to change anything about our information about its files. */
        if (libraryList[item_id]->itemState() == LibraryMixerItem::MATCHED_TO_FILE ||
            libraryList[item_id]->itemState() == LibraryMixerItem::MATCHED_TO_LEND ||
            libraryList[item_id]->itemState() == LibraryMixerItem::MATCH_NOT_FOUND) {

            for (int i = 0; i < libraryList[item_id]->fileCount(); i++) {
                if (path == libraryList[item_id]->paths()[i]) {
                    /* When a file goes missing, we clear out its hash information and it will be rehashed on next library update when its present.
                       This could be bad if a whole large set of files goes missing, but that should be fairly uncommon.
                       This is because for better or worse QFileSystemWatcher doesn't detect drive removals or network share disconnections.
                       Therefore, while it may be ideal to keep around old hash information so we can send it with the hash job when the file returns,
                       this works well enough for now and is a lot easier than otherwise individually tracking which files are missing. */
                    libraryList[item_id]->setFileInfo(path, -1, 0, "");
                    libraryList[item_id]->itemState(LibraryMixerItem::MATCH_NOT_FOUND);
                    emit libraryStateChanged(item_id);
                    toSave = true;
                }
            }
        }
    }
    if (toSave) XmlUtil::writeXml(LIBRARYFILE, xml);
}

//private utility functions

LibraryMixerItem LibraryMixerLibraryManager::internalConvertItem(LibraryMixerLibraryItem *inputItem) const {
    LibraryMixerItem returnItem;
    returnItem.title = inputItem->title();
    returnItem.item_id = inputItem->id();
    returnItem.itemState = inputItem->itemState();

    if (LibraryMixerItem::MATCHED_TO_LENT == returnItem.itemState) {
        returnItem.lentTo = inputItem->lentTo();
    } else if (LibraryMixerItem::MATCHED_TO_MESSAGE == returnItem.itemState) {
        returnItem.message = inputItem->message();
    } else if (LibraryMixerItem::MATCHED_TO_FILE == returnItem.itemState ||
               LibraryMixerItem::MATCH_NOT_FOUND == returnItem.itemState ||
               LibraryMixerItem::MATCHED_TO_LEND == returnItem.itemState ||
               LibraryMixerItem::MATCHED_TO_LENT == returnItem.itemState) {
        returnItem.paths = inputItem->paths();
        returnItem.filesizes = inputItem->filesizes();
        returnItem.hashes = inputItem->hashes();
    }
    return returnItem;
}

bool LibraryMixerLibraryManager::equalNode(const QDomNode a, const QDomNode b) const {
    return !a.firstChildElement("id").isNull() && !b.firstChildElement("id").isNull() &&
           a.firstChildElement("id").text() == b.firstChildElement("id").text();
}
