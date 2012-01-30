/****************************************************************
 *  Copyright 2010, Fair Use, Inc.
 *  Copyright 2004-6, Robert Fernie
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

#include "util/dir.h"
#include "interface/iface.h"
#include "interface/peers.h"
#include "pqi/pqinotify.h"

#include "services/p3chatservice.h"

/****
 * #define CHAT_DEBUG 1
 ****/

/* This Service is so simple that there is no
 * mutex protection required! */

p3ChatService::p3ChatService()
    :p3Service(SERVICE_TYPE_CHAT) {
    addSerialType(new ChatSerialiser());
    _own_avatar = NULL ;
}

int p3ChatService::tick() {
    return 0;
}

int p3ChatService::status() {
    return 1;
}

/***************** Chat Stuff **********************/

class p3ChatService::AvatarInfo {
public:
    AvatarInfo() {
        _image_size = 0 ;
        _image_data = NULL ;
        _peer_is_new = false ;  // true when the peer has a new avatar
        _own_is_new = false ;   // true when I myself have a new avatar to send to this peer.
    }

    ~AvatarInfo() {
        delete[] _image_data ;
        _image_data = NULL ;
        _image_size = 0 ;
    }

    AvatarInfo(const AvatarInfo &ai) {
        init(ai._image_data,ai._image_size) ;
    }

    void init(const unsigned char *jpeg_data,int size) {
        _image_size = size ;
        _image_data = new unsigned char[size] ;
        memcpy(_image_data,jpeg_data,size) ;
    }
    AvatarInfo(const unsigned char *jpeg_data,int size) {
        init(jpeg_data,size) ;
    }
    void toUnsignedChar(unsigned char *& data,uint32_t &size) const {
        data = new unsigned char[_image_size] ;
        size = _image_size ;
        memcpy(data,_image_data,size*sizeof(unsigned char)) ;
    }

    uint32_t _image_size ;
    unsigned char *_image_data ;
    int _peer_is_new ;          // true when the peer has a new avatar
    int _own_is_new ;           // true when I myself a new avatar to send to this peer.
};


void    p3ChatService::sendStatusString( const std::string &id , const QString &status_string) {
    ChatStatusItem *cs = new ChatStatusItem ;

    cs->status_string = status_string ;
    cs->PeerId(id);

#ifdef CHAT_DEBUG
    std::cerr  << "sending chat status packet:" << std::endl ;
    cs->print(std::cerr) ;
#endif
    sendItem(cs);
}

int     p3ChatService::sendPrivateChat(const std::string &peer_id, const QString &message) {
    // make chat item....
#ifdef CHAT_DEBUG
    std::cerr << "p3ChatService::sendPrivateChat()";
    std::cerr << std::endl;
#endif

    ChatMsgItem *ci = new ChatMsgItem();

    ci->PeerId(peer_id);
    ci->chatFlags = CHAT_FLAG_PRIVATE;
    ci->sendTime = time(NULL);
    ci->message = message;

    std::map<std::string,AvatarInfo *>::iterator it = _avatars.find(peer_id) ;

    if (it == _avatars.end()) {
        _avatars[peer_id] = new AvatarInfo ;
        it = _avatars.find(peer_id) ;
    }
    if (it != _avatars.end() && it->second->_own_is_new) {
#ifdef CHAT_DEBUG
        std::cerr << "p3ChatService::sendPrivateChat: new avatar never sent to peer " << peer_id << ". Setting <new> flag to packet." << std::endl;
#endif

        ci->chatFlags |= CHAT_FLAG_AVATAR_AVAILABLE ;
        it->second->_own_is_new = false ;
    }

#ifdef CHAT_DEBUG
    std::cerr << "Sending msg to peer " << peer_id << ", flags = " << ci->chatFlags << std::endl ;
    std::cerr << "p3ChatService::sendPrivateChat() Item:";
    std::cerr << std::endl;
    ci->print(std::cerr);
    std::cerr << std::endl;
#endif

    sendItem(ci);

    return 1;
}

std::list<ChatMsgItem *> p3ChatService::getChatQueue() {
    time_t now = time(NULL);

    NetItem *item ;
    std::list<ChatMsgItem *> ilist;

    while (NULL != (item=recvItem())) {
#ifdef CHAT_DEBUG
        std::cerr << "p3ChatService::getChatQueue() Item:" << (void *)item << std::endl ;
#endif
        ChatMsgItem *ci = dynamic_cast<ChatMsgItem *>(item) ;

        if (ci != NULL) {
#ifdef CHAT_DEBUG
            std::cerr << "p3ChatService::getChatQueue() Item:";
            std::cerr << std::endl;
            ci->print(std::cerr);
            std::cerr << std::endl;
            std::cerr << "Got msg. Flags = " << ci->chatFlags << std::endl ;
#endif

            if (ci->chatFlags & CHAT_FLAG_CONTAINS_AVATAR) {// no msg here. Just an avatar.
                delete item ;
                continue ;
            } else if (ci->chatFlags & CHAT_FLAG_REQUESTS_AVATAR) {// no msg here. Just an avatar request.
                sendAvatarJpegData(ci->PeerId()) ;
                delete item ;
                continue ;
            } else {// normal msg.
                // Check if new avatar is available at peer's. If so, send a request to get the avatar.
                if (ci->chatFlags & CHAT_FLAG_AVATAR_AVAILABLE) {
                    //New avatar is available for peer, send request
                    sendAvatarRequest(ci->PeerId()) ;
                    ci->chatFlags &= ~CHAT_FLAG_AVATAR_AVAILABLE ;
                }

                std::map<std::string,AvatarInfo *>::const_iterator it = _avatars.find(ci->PeerId()) ;
                if (it!=_avatars.end() && it->second->_peer_is_new) {
                    ci->chatFlags |= CHAT_FLAG_AVATAR_AVAILABLE ;
                }

                ci->recvTime = now;
                ilist.push_back(ci);
            }
        }
        ChatStatusItem *cs = dynamic_cast<ChatStatusItem *>(item) ;

        if (cs != NULL) {
            // we should notify for a status string for the current peer.
#ifdef CHAT_DEBUG
            std::cerr << "Received status string \"" << cs->status_string.toStdString() << "\"" << std::endl ;
#endif
            notifyBase->notifyChatStatus(peers->findLibraryMixerByCertId(cs->PeerId()),
                                                  cs->status_string) ;

            delete item ;
            continue ;
        }

        ChatAvatarItem *ca = dynamic_cast<ChatAvatarItem *>(item) ;

        if (ca != NULL) {
            receiveAvatarJpegData(ca) ;
            delete item ;
            continue ;
        }
    }

    return ilist;
}

void p3ChatService::setOwnAvatarJpegData(const unsigned char *data,int size) {
    {
        QMutexLocker stack(&mChatMtx);

        if (_own_avatar != NULL)
            delete _own_avatar ;

        _own_avatar = new AvatarInfo(data,size) ;

        // set the info that our avatar is new, for all peers
        for (std::map<std::string,AvatarInfo *>::iterator it(_avatars.begin()); it!=_avatars.end(); ++it)
            it->second->_own_is_new = true ;
    }
}

void p3ChatService::receiveAvatarJpegData(ChatAvatarItem *ci) {
    QMutexLocker stack(&mChatMtx);

    bool new_peer = (_avatars.find(ci->PeerId()) == _avatars.end()) ;

    _avatars[ci->PeerId()] = new AvatarInfo(ci->image_data,ci->image_size) ;
    _avatars[ci->PeerId()]->_peer_is_new = true ;
    _avatars[ci->PeerId()]->_own_is_new = new_peer ;
}

void p3ChatService::getOwnAvatarJpegData(unsigned char *& data,int &size) {
    // should be a Mutex here.
    QMutexLocker stack(&mChatMtx);

    uint32_t s = 0 ;
    if (_own_avatar != NULL) {
        _own_avatar->toUnsignedChar(data,s) ;
        size = s ;
    } else {
        data=NULL ;
        size=0 ;
    }
}
void p3ChatService::getAvatarJpegData(const std::string &peer_id,unsigned char *& data,int &size) {
    // should be a Mutex here.
    QMutexLocker stack(&mChatMtx);

    std::map<std::string,AvatarInfo *>::const_iterator it = _avatars.find(peer_id) ;

    if (it!=_avatars.end()) {
        uint32_t s=0 ;
        it->second->toUnsignedChar(data,s) ;
        size = s ;
        it->second->_peer_is_new = false ;
        return ;
    }

    sendAvatarRequest(peer_id) ;
}

void p3ChatService::sendAvatarRequest(const std::string &peer_id) {
    // Doesn't have avatar. Request it.
    //
    ChatMsgItem *ci = new ChatMsgItem();

    ci->PeerId(peer_id);
    ci->chatFlags = CHAT_FLAG_PRIVATE | CHAT_FLAG_REQUESTS_AVATAR ;
    ci->sendTime = time(NULL);
    ci->message = "";

    sendItem(ci);
}

ChatAvatarItem *p3ChatService::makeOwnAvatarItem() {
    QMutexLocker stack(&mChatMtx);
    ChatAvatarItem *ci = new ChatAvatarItem();

    _own_avatar->toUnsignedChar(ci->image_data,ci->image_size) ;

    return ci ;
}


void p3ChatService::sendAvatarJpegData(const std::string &peer_id) {
    if (_own_avatar != NULL) {
        ChatAvatarItem *ci = makeOwnAvatarItem();
        ci->PeerId(peer_id);

        // take avatar, and embed it into a std::wstring.
        sendItem(ci) ;
    }
}
