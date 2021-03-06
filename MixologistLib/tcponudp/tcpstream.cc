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


#include <stdlib.h>
#include <string.h>

#include "tcpstream.h"
#include <iostream>
#include <sstream>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <limits.h>

#include <sys/time.h>
#include <time.h>

#include "util/debug.h"

/*
 * #define TCP_NO_PARTIAL_READ 1
 */

static const uint32 kMaxQueueSize = 100;
static const uint32 kMaxPktRetransmit = 20;
static const uint32 kMaxSynPktRetransmit = 1000; // up to 1000 (16 min?) startup
static const int TCP_STD_TTL = 64;
static const int TCP_DEFAULT_FIREWALL_TTL = 4;

static const double RTT_ALPHA = 0.875;

// platform independent fractional timestamp.
static double getCurrentTS();

TcpStream::TcpStream(UdpSorter *lyr)
    :inSize(0), outSizeRead(0), outSizeNet(0),
     state(TCP_CLOSED),
     inStreamActive(false),
     outStreamActive(false),
     outSeqno(0), outAcked(0), outWinSize(0),
     inAckno(0), inWinSize(0),
     maxWinSize(TCP_MAX_WIN),
     keepAliveTimeout(TCP_ALIVE_TIMEOUT),
     retransTimeout(TCP_RETRANS_TIMEOUT),
     lastWriteTF(0),lastReadTF(0),
     wcount(0), rcount(0),
     errorState(0),
 /* retranmission variables - init to large */
     rtt_est(TCP_RETRANS_TIMEOUT),
     rtt_dev(0),
     congestThreshold(TCP_MAX_WIN),
     congestWinSize(MAX_SEG),
     congestUpdate(0),
     mTTL_period(0),
     mTTL_start(0),
     mTTL_end(0),
     peerKnown(false),
     udp(lyr) {
    return;
}

/* Stream Control! */
int TcpStream::connect(const struct sockaddr_in &raddr, uint32_t conn_period) {
    QMutexLocker stack(&tcpMtx);

    setRemoteAddress(raddr);

    /* check state */
    if (state != TCP_CLOSED) {
        if (state == TCP_ESTABLISHED) return 0;
        else if (state < TCP_ESTABLISHED) errorState = EAGAIN;
        else errorState = EFAULT;
        return -1;
    }

    /* setup Seqnos */
    outSeqno = genSequenceNo();
    initOurSeqno = outSeqno;

    outAcked = outSeqno; /* min - 1 expected */
    inWinSize = maxWinSize;

    congestThreshold = TCP_MAX_WIN;
    congestWinSize = MAX_SEG;
    congestUpdate = outAcked + congestWinSize;

    /* Init Connection */
    /* send syn packet */
    TcpPacket *pkt = new TcpPacket();
    pkt->setSyn();

    /* ********* SLOW START *************
     * As this is the only place where a syn
     * is sent ..... we switch the ttl to 0,
     * and increment it as we retransmit the packet....
     * This should help the firewalls along.
     * ******
     * Note about above: This is what they had here, I can't say I understand the rationale:
     *     setTTL(1);
     * I can say that by setting a short TTL, we were unable to connect over the Internet.
     * By now setting a normal TTL, we can now connect over the Internet.
     * Not sure what the benefits of helping firewalls they were suggesting might have been.
     */

    setTTL(TCP_STD_TTL);

    mTTL_start  = getCurrentTS();
    mTTL_period = conn_period;
    mTTL_end    = mTTL_start + mTTL_period;

    toSend(pkt);
    /* change state */
    state = TCP_SYN_SENT;
    errorState = EAGAIN;

    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::connect state => TCP_SYN_SENT");

    return -1;
}


int TcpStream::listenfor(const struct sockaddr_in &raddr) {
    QMutexLocker stack(&tcpMtx);

    setRemoteAddress(raddr);

    /* check state */
    if (state != TCP_CLOSED) {
        if (state == TCP_ESTABLISHED) return 0;
        else if (state < TCP_ESTABLISHED) errorState = EAGAIN;
        else errorState = EFAULT;
        return -1;
    }

    errorState = EAGAIN;
    return -1;
}


/* Stream Control! */
int TcpStream::close() {
    QMutexLocker stack(&tcpMtx);

    cleanup();

    return 0;
}

int TcpStream::reset() {
    QMutexLocker stack(&tcpMtx);

    TcpPacket *pkt = new TcpPacket();
    pkt->setRst();
    toSend(pkt);

    return 0;
}

bool TcpStream::isConnected() {
    QMutexLocker stack(&tcpMtx);

    return (state == TCP_ESTABLISHED);
}

int TcpStream::status(std::ostream &out) {
    QMutexLocker stack(&tcpMtx);

    int tmpstate = state;

    // can leave the timestamp here as time()... rough but okay.
    out << "TcpStream::status @ (" << time(NULL) << ")" << std::endl;
    out << "TcpStream::state = " << (int) state << std::endl;
    out << std::endl;
    out << "writeBuffer: " << inSize << " + 1500 * " << inQueue.size();
    out << " bytes Queued for transmission" << std::endl;
    out << "readBuffer: " << outSizeRead << " + 1500 * ";
    out << outQueue.size() << " + " << outSizeNet;
    out << " incoming bytes waiting" << std::endl;
    out << std::endl;
    out << "inPkts: " << inPkt.size() << " packets waiting for processing";
    out << std::endl;
    out << "outPkts: " << outPkt.size() << " packets waiting for acks";
    out << std::endl;
    out << "us->peer: nextSeqno: " << outSeqno << " lastAcked: " << outAcked;
    out << " winsize: " << outWinSize;
    out << std::endl;
    out << "peer->us: Expected SeqNo: " << inAckno;
    out << " winsize: " << inWinSize;
    out << std::endl;
    out << std::endl;

    return tmpstate;
}

int TcpStream::write_allowed() {
    QMutexLocker stack(&tcpMtx);

    int ret = 1;
    if (state == TCP_CLOSED) {
        errorState = EBADF;
        ret = -1;
    } else if (state < TCP_ESTABLISHED) {
        errorState = EAGAIN;
        ret = -1;
    } else if (!outStreamActive) {
        errorState = EBADF;
        ret = -1;
    }

    if (ret < 1) return ret;

    int maxwrite = (kMaxQueueSize -  inQueue.size()) * MAX_SEG;

    return maxwrite;
}

int TcpStream::read_pending() {
    QMutexLocker stack(&tcpMtx);

    /* error should be detected next time */
    int maxread = int_read_pending();
    if (state == TCP_CLOSED) {
        errorState = EBADF;
        maxread = -1;
    } else if (state < TCP_ESTABLISHED) {
        errorState = EAGAIN;
        maxread = -1;
    }

    return maxread;
}

/* INTERNAL */
int TcpStream::int_read_pending() {
    return outSizeRead + outQueue.size() * MAX_SEG + outSizeNet;
}


/* stream Interface */
int TcpStream::write(char *dta, int size) { /* write->pkt->net */
    QMutexLocker stack(&tcpMtx);
    int ret = 1; /* initial error checking */


    if (state == TCP_CLOSED) {
        errorState = EBADF;
        ret = -1;
    } else if (state < TCP_ESTABLISHED) {
        errorState = EAGAIN;
        ret = -1;
    } else if (inQueue.size() > kMaxQueueSize) {
        errorState = EAGAIN;
        ret = -1;
    } else if (!outStreamActive) {
        errorState = EBADF;
        ret = -1;
    }

    if (ret < 1) { /* check for initial error */
        return ret;
    }

    if (size + inSize < MAX_SEG) {
        memcpy((void *) &(inData[inSize]), dta, size);
        inSize += size;

        return size;
    }

    /* otherwise must construct a dataBuffer. */

    /* first create 1. */
    dataBuffer *db = new dataBuffer;
    memcpy((void *) db->data, (void *) inData, inSize);


    int remSize = size;
    memcpy((void *) &(db->data[inSize]), dta, MAX_SEG - inSize);

    inQueue.push_back(db);
    remSize -= (MAX_SEG - inSize);

    while (remSize >= MAX_SEG) {
        db = new dataBuffer;
        memcpy((void *) db->data, (void *) &(dta[size-remSize]), MAX_SEG);

        inQueue.push_back(db);
        remSize -= MAX_SEG;
    }

    if (remSize > 0) {
        memcpy((void *) inData, (void *) &(dta[size-remSize]), remSize);
        inSize = remSize;
    } else {
        inSize = 0;
    }

    return size;
}

int TcpStream::read(char *dta, int size) { /* net->  pkt->read */
    QMutexLocker stack(&tcpMtx);

    /* max available data is
     * outDataRead + outQueue + outDataNet
     */

    int maxread = outSizeRead + outQueue.size() * MAX_SEG + outSizeNet;
    int ret = 1; /* used only for initial errors */

    if (state == TCP_CLOSED) {
        errorState = EBADF;
        ret = -1;
    } else if (state < TCP_ESTABLISHED) {
        errorState = EAGAIN;
        ret = -1;
    } else if ((!inStreamActive) && (maxread == 0)) {
        // finished stream.
        ret = 0;
    } else if (maxread == 0) {
        /* must wait for more data */
        errorState = EAGAIN;
        ret = -1;
    }

    if (ret < 1) return ret;

    if (maxread < size) {
#ifdef TCP_NO_PARTIAL_READ
        if (inStreamActive) {
            errorState = EAGAIN;
            return -1;
        }
#endif /* TCP_NO_PARTIAL_READ */
        size = maxread;
    }

    /* if less than outDataRead size */
    if (((unsigned) (size) < outSizeRead) && (outSizeRead)) {
        memcpy(dta,(void *) outDataRead, size);
        memmove((void *) outDataRead,
                (void *) &(outDataRead[size]), outSizeRead - (size));
        outSizeRead -= size;

        /* can allow more in! - update inWinSize */
        UpdateInWinSize();

        return size;
    }

    /* move the whole of outDataRead. */
    if (outSizeRead) {
        memcpy(dta,(void *) outDataRead, outSizeRead);
    }

    int remSize = size - outSizeRead;
    outSizeRead = 0;

    while ((outQueue.size() > 0) && (remSize > 0)) {
        dataBuffer *db = outQueue.front();
        outQueue.pop_front(); /* remove */

        /* load into outDataRead */
        if (remSize < MAX_SEG) {
            memcpy((void *) &(dta[(size)-remSize]), (void *) db->data, remSize);
            memcpy((void *) outDataRead, (void *) &(db->data[remSize]), MAX_SEG - remSize);
            outSizeRead = MAX_SEG - remSize;

            delete db;

            /* can allow more in! - update inWinSize */
            UpdateInWinSize();

            return size;
        }

        /* else copy whole segment */
        memcpy((void *) &(dta[(size)-remSize]), (void *) db->data, MAX_SEG);
        remSize -= MAX_SEG;
        delete db;
    }

    /* assumes that outSizeNet >= remSize due to initial
     * constraint
     */
    if ((remSize > 0)) {
        memcpy((void *) &(dta[(size)-remSize]),(void *) outDataNet, remSize);
        outSizeNet -= remSize;
        if (outSizeNet > 0) {
            /* move to the outDataRead */
            memcpy((void *) outDataRead,(void *) &(outDataNet[remSize]), outSizeNet);
            outSizeRead = outSizeNet;
            outSizeNet = 0;
        }

        /* can allow more in! - update inWinSize */
        UpdateInWinSize();

        return size;
    }

    /* can allow more in! - update inWinSize */
    UpdateInWinSize();

    return size;
}


/* Callback from lower Layers */
void TcpStream::recvPkt(void *data, int size) {
    QMutexLocker stack(&tcpMtx);
    uint8 *input = (uint8 *) data;

    TcpPacket *pkt = new TcpPacket();
    if (0 < pkt->readPacket(input, size)) {
        lastIncomingPkt = getCurrentTS();
        handleIncoming(pkt);
    } else {
        delete pkt;
    }
}


int TcpStream::tick() {
    QMutexLocker stack(&tcpMtx);

    //std::cerr << "TcpStream::tick()" << std::endl;
    recv_check(); /* recv is async */
    send();

    return 1;
}

bool TcpStream::getRemoteAddress(struct sockaddr_in &raddr) {
    QMutexLocker stack(&tcpMtx);

    if (peerKnown) {
        raddr = peeraddr;
    }

    return peerKnown;
}

uint8 TcpStream::TcpState() {
    QMutexLocker stack(&tcpMtx);

    return state;
}

int TcpStream::TcpErrorState() {
    QMutexLocker stack(&tcpMtx);

    return errorState;
}



/********************* SOME EXPOSED DEBUGGING FNS ******************/

static int ilevel = 100;

bool TcpStream::widle() {
    QMutexLocker stack(&tcpMtx);
    /* init */
    if (!lastWriteTF) {
        lastWriteTF = int_wbytes();
        return false;
    }

    if ((lastWriteTF == int_wbytes()) && (inSize + inQueue.size() == 0)) {
        wcount++;
        if (wcount > ilevel) return true;
        else return false;
    }
    wcount = 0;
    lastWriteTF = int_wbytes();

    return false;
}


bool TcpStream::ridle() {
    QMutexLocker stack(&tcpMtx);
    /* init */
    if (!lastReadTF) {
        lastReadTF = int_rbytes();
        return false;
    }

    if ((lastReadTF == int_rbytes()) && (outSizeRead + outQueue.size() + outSizeNet== 0)) {
        rcount++;
        if (rcount > ilevel) return true;
        else return false;
    }
    rcount = 0;
    lastReadTF = int_rbytes();
    return false;
}

uint32 TcpStream::wbytes() {
    QMutexLocker stack(&tcpMtx);
    return int_wbytes();
}

uint32 TcpStream::rbytes() {
    QMutexLocker stack(&tcpMtx);
    return int_rbytes();
}

/********************* ALL BELOW HERE IS INTERNAL ******************
 ******************* AND ALWAYS PROTECTED BY A MUTEX ***************/

int TcpStream::recv_check() {
    double cts = getCurrentTS(); // fractional seconds.

    // make sure we've rcvd something!
    if ((state > TCP_SYN_RCVD) &&
        (cts - lastIncomingPkt > kNoPktTimeout)) {
        /* shut it all down */
        /* this period should be equivalent
         * to the firewall timeouts ???
         *
         * for max efficiency
         */

        outStreamActive = false;
        inStreamActive = false;
        state = TCP_CLOSED;
        log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::recv_check state => TCP_CLOSED");
        cleanup();
    }
    return 1;
}

int TcpStream::cleanup() {
    // This shuts it all down! no matter what.
    outStreamActive = false;
    inStreamActive = false;
    state = TCP_CLOSED;
    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::cleanup state => TCP_CLOSED");

    //peerKnown = false; //??? NOT SURE->for a rapid reconnetion this might be key??

    /* reset TTL */
    setTTL(TCP_STD_TTL);

    // clear arrays.
    inSize = 0;
    while (inQueue.size() > 0) {
        dataBuffer *db = inQueue.front();
        inQueue.pop_front();
        delete db;
    }

    while (outPkt.size() > 0) {
        TcpPacket *pkt = outPkt.front();
        outPkt.pop_front();
        delete pkt;
    }


    // clear arrays.
    outSizeRead = 0;
    outSizeNet = 0;
    while (outQueue.size() > 0) {
        dataBuffer *db = outQueue.front();
        outQueue.pop_front();
        delete db;
    }

    while (inPkt.size() > 0) {
        TcpPacket *pkt = inPkt.front();
        inPkt.pop_front();
        delete pkt;
    }
    return 1;
}

int TcpStream::handleIncoming(TcpPacket *pkt) {
    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "Handling incoming packet, current state is: " + QString::number(state));
    switch (state) {
        case TCP_CLOSED:
        case TCP_LISTEN:
            /* if receive SYN
             *->respond SYN/ACK
             * To State: SYN_RCVD
             *
             * else Discard.
             */
            return incoming_Closed(pkt);
            break;
        case TCP_SYN_SENT:
            /* if receive SYN
             *->respond SYN/ACK
             * To State: SYN_RCVD
             *
             * if receive SYN+ACK
             *->respond ACK
             * To State: TCP_ESTABLISHED
             *
             * else Discard.
             */
            return incoming_SynSent(pkt);
            break;
        case TCP_SYN_RCVD:
            /* if receive ACK
             * To State: TCP_ESTABLISHED
             */
            return incoming_SynRcvd(pkt);
            break;
        case TCP_ESTABLISHED:
            /* if receive FIN
             *->respond ACK
             * To State: TCP_CLOSE_WAIT
             * else Discard.
             */
            return incoming_Established(pkt);
            break;
        case TCP_FIN_WAIT_1:
            /* state entered by close() call.
             * if receive FIN
             *->respond ACK
             * To State: TCP_CLOSING
             *
             * if receive ACK
             *->no response
             * To State: TCP_FIN_WAIT_2
             *
             * if receive FIN+ACK
             *->respond ACK
             * To State: TCP_TIMED_WAIT
             *
             */
            return incoming_Established(pkt);
            //return incoming_FinWait1(pkt);
            break;
        case TCP_FIN_WAIT_2:
            /* if receive FIN
             *->respond ACK
             * To State: TCP_TIMED_WAIT
             */
            return incoming_Established(pkt);
            //return incoming_FinWait2(pkt);
            break;
        case TCP_CLOSING:
            /* if receive ACK
             * To State: TCP_TIMED_WAIT
             */
            /* all handled in Established */
            return incoming_Established(pkt);
            //return incoming_Closing(pkt);
            break;
        case TCP_CLOSE_WAIT:
            /*
             * wait for our close to be called.
             */
            /* all handled in Established */
            return incoming_Established(pkt);
            //return incoming_CloseWait(pkt);
            break;
        case TCP_LAST_ACK:
            /* entered by the local close() after sending FIN.
             * if receive ACK
             * To State: TCP_CLOSED
             */
            /* all handled in Established */
            return incoming_Established(pkt);
            /*
            return incoming_LastAck(pkt);
             */

            break;
            /* this is actually the only
             * final state where packets not expected!
             */
        case TCP_TIMED_WAIT:
            /* State: TCP_TIMED_WAIT
             *
             * discard all->both connections FINed
             * timeout of this state.
             *
             */
            state = TCP_CLOSED;
            // return incoming_TimedWait(pkt);
            {
                std::ostringstream out;
                out << "TcpStream::state => TCP_CLOSED";
                out << " (recvd TCP_TIMED_WAIT?)";
                log(LOG_WARNING, TCP_STREAM_ZONE, out.str().c_str());
            }
            break;
    }
    delete pkt;
    return 1;
}


int TcpStream::incoming_Closed(TcpPacket *pkt) {
    /* if receive SYN
     *->respond SYN/ACK
     * To State: SYN_RCVD
     *
     * else Discard.
     */

    if ((pkt->hasSyn()) && (!pkt->hasAck())) {
        /* Init Connection */

        /* save seqno */
        initPeerSeqno = pkt->seqno;
        inAckno = initPeerSeqno + 1;
        outWinSize = pkt->winsize;


        inWinSize = maxWinSize;

        /* we can get from SynSent as well,
         * but only send one SYN packet
         */

        /* start packet */
        TcpPacket *rsp = new TcpPacket();

        if (state == TCP_CLOSED) {
            outSeqno = genSequenceNo();
            initOurSeqno = outSeqno;
            outAcked = outSeqno; /* min - 1 expected */

            /* setup Congestion Charging */
            congestThreshold = TCP_MAX_WIN;
            congestWinSize   = MAX_SEG;
            congestUpdate    = outAcked + congestWinSize;

            rsp->setSyn();
        }

        rsp->setAck(inAckno);
        /* seq + winsize set in toSend() */

        /* as we have received something ... we can up the TTL */
        setTTL(TCP_STD_TTL);

        toSend(rsp);
        /* change state */
        state = TCP_SYN_RCVD;
        log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::incoming_Closed state => TCP_SYN_RCVD");
    }

    delete pkt;
    return 1;
}


int TcpStream::incoming_SynSent(TcpPacket *pkt) {
    /* if receive SYN
     *->respond SYN/ACK
     * To State: SYN_RCVD
     *
     * if receive SYN+ACK
     *->respond ACK
     * To State: TCP_ESTABLISHED
     *
     * else Discard.
     */

    if ((pkt->hasSyn()) && (pkt->hasAck())) {
        /* check stuff */
        if (pkt->getAck() != outSeqno) {
            log(LOG_DEBUG_ALERT, TCP_STREAM_ZONE, "TcpStream::incoming_SynSent() Bad Ack - " + QString::number(pkt->getAck()));
            delete pkt;
            return -1;
        }

        /* Complete Connection */

        /* save seqno */
        initPeerSeqno = pkt->seqno;
        inAckno = initPeerSeqno + 1;

        outWinSize = pkt->winsize;

        outAcked = pkt->getAck();

        /* before ACK, reset the TTL
         * As they have sent something, and we have received
         * through the firewall, set to STD.
         */
        setTTL(TCP_STD_TTL);

        /* ack the Syn Packet */
        sendAck();

        /* change state */
        state = TCP_ESTABLISHED;
        outStreamActive = true;
        inStreamActive = true;

        log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::incoming_SynSent state => TCP_ESTABLISHED");

        delete pkt;
    } else { /* same as if closed! (simultaneous open) */
        return incoming_Closed(pkt);
    }
    return 1;
}


int TcpStream::incoming_SynRcvd(TcpPacket *pkt) {
    /* if receive ACK
     * To State: TCP_ESTABLISHED
     */

    if (pkt->hasRst()) {
        state = TCP_CLOSED;
        log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::incoming_SynRcvd state => TCP_CLOSED");
        delete pkt;
        return 1;
    }

    bool ackWithData = false;

    if (pkt->hasAck()) {
        if (pkt->hasSyn()) {
            /* has resent syn->check it matches */
        }

        /* check stuff */
        if (pkt->getAck() != outSeqno) {
            /* bad ignore */
            delete pkt;
            return -1;
        }

        /* Complete Connection */

        /* save seqno */
        if (pkt->datasize > 0) {
            // managed to trigger this under windows...
            // perhaps the initial Ack was lost,
            // believe we should just pass this packet
            // directly to the incoming_Established... once
            // the following has been done.
            // and it should all work!
            //exit(1);
            ackWithData = true;
        }

        inAckno = pkt->seqno; /* + pkt->datasize; */
        outWinSize = pkt->winsize;

        outAcked = pkt->getAck();


        /* As they have sent something, and we have received
         * through the firewall, set to STD.
         */
        setTTL(TCP_STD_TTL);

        /* change state */
        state = TCP_ESTABLISHED;
        outStreamActive = true;
        inStreamActive = true;
        log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::incoming_SynRcvd state => TCP_ESTABLISHED");
    }

    if (ackWithData) {
        /* connection Established->handle normally */
        incoming_Established(pkt);
    } else {
        /* else nothing */
        delete pkt;
    }
    return 1;
}

int TcpStream::incoming_Established(TcpPacket *pkt) {
    /* first handle the Ack ...
     * this must be done before the queue,
     * to keep the values as up-to-date as possible.
     *
     * must sanity check .....
     * make sure that the sequence number is within the correct range.
     */

    if (pkt->hasRst()) {
        state = TCP_CLOSED;
        log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::incoming_Established state => TCP_CLOSED");
        delete pkt;
        return 1;
    }

    if ((!isOldSequence(pkt->seqno, inAckno)) &&           // seq >= inAckno
            isOldSequence(pkt->seqno, inAckno + maxWinSize)) { // seq < inAckno + maxWinSize.
        if (pkt->hasAck()) {
            outAcked = pkt->ackno;
        }

        outWinSize = pkt->winsize;
    } else {
        sendAck();
    }


    /* add to queue */
    inPkt.push_back(pkt);

    if (inPkt.size() > kMaxQueueSize) {
        TcpPacket *pkt = inPkt.front();
        inPkt.pop_front();
        delete pkt;
    }

    /* use as many packets as possible */
    return check_InPkts();
}

int TcpStream::check_InPkts() {
    bool found = true;
    TcpPacket *pkt;
    std::list<TcpPacket *>::iterator it;
    while (found) {
        found = false;
        for (it = inPkt.begin(); (!found) && (it != inPkt.end());) {
            pkt = *it;
            if ((*it)->seqno == inAckno) {
                found = true;
                inPkt.erase(it);

            }

            /* see if we can discard it */
            /* if smaller seqno, and not wrapping around */
            else if (isOldSequence((*it)->seqno, inAckno)) {
                /* discard */
                it = inPkt.erase(it);
                delete pkt;

            } else {
                it++;
            }
        }
        if (found) {
            /* update ack number - let it rollover */
            inAckno = pkt->seqno + pkt->datasize;

            /* XXX This shouldn't be here, as it prevents
             * the Ack being used until the packet is.
             * This means that a dropped packet will stop traffic in both
             * directions....
             *
             * Moved it to incoming_Established .... but extra
             * check here to be sure!
             */

            if (pkt->hasAck()) {
                if (isOldSequence(outAcked, pkt->ackno)) {
                    outAcked = pkt->ackno;
                    outWinSize = pkt->winsize;
                }
            }

            /* push onto queue */

            if (outSizeNet + pkt->datasize < MAX_SEG) {
                /* move onto outSizeNet */
                if (pkt->datasize) {
                    memcpy((void *) &(outDataNet[outSizeNet]), pkt->data, pkt->datasize);
                    outSizeNet += pkt->datasize;
                }
            } else {
                /* if it'll overflow the buffer. */
                dataBuffer *db = new dataBuffer();

                /* move outDatNet->buffer */
                memcpy((void *) db->data, (void *) outDataNet, outSizeNet);

                /* fill rest of space */
                int remSpace = MAX_SEG - outSizeNet;
                memcpy((void *) &(db->data[outSizeNet]), (void *) pkt->data, remSpace);

                /* remove any remaining to outDataNet */
                outSizeNet = pkt->datasize - remSpace;
                if (outSizeNet > 0) {
                    memcpy((void *) outDataNet, (void *) &(pkt->data[remSpace]), outSizeNet);
                }

                /* push packet onto queue */
                outQueue.push_back(db);
            }

            /* can allow more in! - update inWinSize */
            UpdateInWinSize();

            /* if pkt is FIN */
            /* these must be here->at the end of the reliable stream */
            /* if the fin is set, ack it specially close stream */
            if (pkt->hasFin()) {
                /* send final ack */
                sendAck();

                /* closedown stream */
                inStreamActive = false;

                if (state == TCP_ESTABLISHED) {
                    state = TCP_CLOSE_WAIT;
                    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::check_InPkts state => TCP_CLOSE_WAIT");
                } else if (state == TCP_FIN_WAIT_1) {
                    state = TCP_CLOSING;
                    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::check_InPkts state => TCP_CLOSING");
                } else if (state == TCP_FIN_WAIT_2) {
                    state = TCP_TIMED_WAIT;
                    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::check_InPkts state => TCP_TIMED_WAIT");
                    cleanup();
                }
            }

            /* if ack for our FIN */
            if ((pkt->hasAck()) && (!outStreamActive)
                    && (pkt->ackno == outSeqno)) {
                if (state == TCP_FIN_WAIT_1) {
                    state = TCP_FIN_WAIT_2;
                    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::check_InPkts state => TCP_FIN_WAIT2");
                } else if (state == TCP_LAST_ACK) {
                    state = TCP_CLOSED;
                    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::check_InPkts state => TCP_CLOSED");
                    cleanup();
                } else if (state == TCP_CLOSING) {
                    state = TCP_TIMED_WAIT;
                    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::check_InPkts state => TCP_TIMED_WAIT");
                    cleanup();
                }
            }

            delete pkt;

        } /* end of found */
    } /* while(found) */
    return 1;
}

/* This Fn should be called after each read, or recvd data (thats added to the buffer)
 */
int TcpStream::UpdateInWinSize() {
    /* InWinSize = maxWinSze - QueuedData,
     * actually we can allow a lot more to queue up...
     * inWinSize = 65536, unless QueuedData > 65536.
     *  inWinSize = 2 * maxWinSize - QueuedData;
     *
     */

    uint32 queuedData = int_read_pending();
    if (queuedData < maxWinSize) {
        inWinSize = maxWinSize;
    } else if (queuedData < 2 * maxWinSize) {
        inWinSize = 2 * maxWinSize - queuedData;
    } else {
        inWinSize = 0;
    }
    return inWinSize;
}

int TcpStream::sendAck() {
    /* simple->toSend fills in ack/winsize
     * and the rest is history
     */
    return toSend(new TcpPacket(), false);
}

void TcpStream::setRemoteAddress(const struct sockaddr_in &raddr) {
    peeraddr = raddr;
    peerKnown = true;
}


int TcpStream::toSend(TcpPacket *pkt, bool retrans) {
    int  outPktSize = MAX_SEG + TCP_PSEUDO_HDR_SIZE;
    char tmpOutPkt[outPktSize];

    if (!peerKnown) {
        /* Major Error! */
        exit(1);
    }

    /* get accurate timestamp */
    double cts =  getCurrentTS();

    pkt->winsize = inWinSize;
    pkt->seqno = outSeqno;

    /* increment seq no */
    if (pkt->datasize) {
        outSeqno += pkt->datasize;
    }

    if (pkt->hasSyn()) {
        /* should not have data! */
        if (pkt->datasize) {}
        outSeqno++;
    } else {
        /* cannot auto Ack SynPackets */
        pkt->setAck(inAckno);
    }

    pkt->winsize = inWinSize;

    /* store old info */
    lastSentAck = pkt->ackno;
    lastSentWinSize = pkt->winsize;
    keepAliveTimer = cts;

    pkt->writePacket(tmpOutPkt, outPktSize);

    int sentsize = udp->sendPkt(tmpOutPkt, outPktSize, &peeraddr, ttl);
    log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "Sent TCP Stream packet result: " + QString::number(sentsize));

    if (retrans) {
        /* restart timers */
        pkt->ts = cts;
        pkt->retrans = 0;

        outPkt.push_back(pkt);
    } else {
        delete pkt;
    }
    return 1;
}



int TcpStream::retrans() {
    int  outPktSize = MAX_SEG + TCP_PSEUDO_HDR_SIZE;
    char tmpOutPkt[outPktSize];
    bool updateCongestion = true;

    if (!peerKnown) {
        /* Major Error! */
        exit(1);
    }

    /* now retrans */
    double cts =  getCurrentTS();
    std::list<TcpPacket *>::iterator it;
    for (it = outPkt.begin(); (it != outPkt.end()); it++) {
        outPktSize = MAX_SEG + TCP_PSEUDO_HDR_SIZE;
        TcpPacket *pkt = (*it);
        if (cts - pkt->ts > retransTimeout) {

            /* retransmission->adjust the congestWinSize and congestThreshold
             * but only once per cycle
             */
            if (updateCongestion) {
                congestThreshold = congestWinSize / 2;
                congestWinSize = MAX_SEG;
                congestUpdate  = outAcked + congestWinSize; // point when we can up the winSize.
                updateCongestion = false;
            }

            /* before we can retranmit,
             * we need to check that its within the congestWinSize
             *->actually only checking that the start (seqno) is within window!
             */


            if (isOldSequence(outAcked + congestWinSize, pkt->seqno)) {
                /* cannot send .... */
                /* as packets in order, can drop out of the fn now */
                return 0;
            }

            /* update ackno and winsize */
            if (!(pkt->hasSyn())) {
                pkt->setAck(inAckno);
                lastSentAck = pkt->ackno;
            }

            pkt->winsize = inWinSize;
            lastSentWinSize = pkt->winsize;

            keepAliveTimer = cts;

            (*it)->writePacket(tmpOutPkt, outPktSize);

            /* if its a syn packet ** thats been
             * transmitting for a while, maybe
             * we should increase the ttl.
             */

            if ((pkt->hasSyn()) && (getTTL() < TCP_STD_TTL)) {
                /* calculate a new TTL */
                if (mTTL_end > cts) {
                    setTTL(TCP_DEFAULT_FIREWALL_TTL);
                } else {
                    setTTL(getTTL() + 1);
                }

                std::ostringstream out;
                out << "TcpStream::retrans() Startup SYNs ";
                out << "retrans count: " << pkt->retrans;
                out << " New TTL: " << getTTL();

                log(LOG_DEBUG_ALERT, TCP_STREAM_ZONE, out.str().c_str());
            }

            /* catch excessive retransmits
             * - Allow Syn case more....
             * - if not SYN or TTL has reached STD then timeout quickly.
             */

            if ((pkt->hasSyn() && (pkt->retrans > kMaxSynPktRetransmit)) ||
                    (((!pkt->hasSyn()) || (TCP_STD_TTL == getTTL()))
                     && (pkt->retrans > kMaxPktRetransmit))) {
                /* too many attempts close stream */
                outStreamActive = false;
                inStreamActive = false;
                state = TCP_CLOSED;
                log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::retrans state => TCP_CLOSED");
                cleanup();
                return 0;
            }


            udp->sendPkt(tmpOutPkt, outPktSize, &peeraddr, ttl);

            /* restart timers */
            (*it)->ts = cts;
            (*it)->retrans++;

            /* finally - double the retransTimeout ... (Karn's Algorithm)
             * this ensures we don't retransmit all the packets that
             * following a dropped packet!
             *
             * but if we have lots of dropped this ain't going to help much!
             *
             * not doubling retransTimeout, that is can go manic and result
             * in excessive timeouts, and no data flow.
             */
            retransTimeout = 2.0 * (rtt_est + 4.0 * rtt_dev);
        }
    }
    return 1;
}


void TcpStream::acknowledge() {
    /* cleans up acknowledge packets */
    /* packets are pushed back in order */
    std::list<TcpPacket *>::iterator it;
    double cts = getCurrentTS();
    bool updateRTT = true;

    for (it = outPkt.begin(); (it != outPkt.end()) &&
            (isOldSequence((*it)->seqno, outAcked));
            it = outPkt.erase(it)) {
        TcpPacket *pkt = (*it);


        /* adjust the congestWinSize and congestThreshold
         * congestUpdate <= outAcked
         *
         ***/

        if (!isOldSequence(outAcked, congestUpdate)) {
            if (congestWinSize < congestThreshold) {
                /* double it baby! */
                congestWinSize *= 2;
            } else {
                /* linear increase */
                congestWinSize += MAX_SEG;
            }

            if (congestWinSize > maxWinSize) {
                congestWinSize = maxWinSize;
            }

            congestUpdate  = outAcked + congestWinSize; // point when we can up the winSize.
        }


        /* update the RoundTripTime,
         * using Jacobson's values.
         * RTT = a RTT + (1-a) M
         * where
         *  RTT is RoundTripTime estimate.
         *  a = 7/8,
         *  M = time for ack.
         *
         * D = a D + (1 - a) | RTT - M |
         * where
         *  D is approx Deviation.
         *  a,RTT & M are the same as above.
         *
         * Timeout = RTT + 4 * D.
         *
         * And Karn's Algorithm...
         * which says
         *  (1) do not update RTT or D for retransmitted packets.
         *      + the ones that follow .... (the ones whos ack was
         *          delayed by the retranmission)
         *  (2) double timeout, when packets fail. (done in retrans).
         */

        if (pkt->retrans) {
            updateRTT = false;
        }

        if (updateRTT) { /* can use for RTT calc */
            double ack_time = cts - pkt->ts;
            rtt_est = RTT_ALPHA * rtt_est + (1.0 - RTT_ALPHA) * ack_time;
            rtt_dev = RTT_ALPHA * rtt_dev + (1.0 - RTT_ALPHA) * fabs(rtt_est - ack_time);
            retransTimeout = rtt_est + 4.0 * rtt_dev;
        }

        delete pkt;
    }

    /* This is triggered if we have recieved acks for retransmitted packets....
     * In this case we want to reset the timeout, and remove the doubling.
     *
     * If we don't do this, and there have been more dropped packets,
     * the the timeout gets continually doubled. which will virtually stop
     * all communication.
     *
     * This will effectively trigger the retransmission of the next dropped packet.
     */

    if (!updateRTT) {
        retransTimeout = rtt_est + 4.0 * rtt_dev;
    }

    return;
}


int TcpStream::send() {
    /* handle network interface always */
    /* clean up as much as possible */
    acknowledge();
    /* send any old packets */
    retrans();


    if (state < TCP_ESTABLISHED) {
        return -1;
    }

    /* get the inQueue, can send */


    /* determine exactly how much we can send */
    uint32 maxsend = congestWinSize;
    uint32 inTransit;

    if (outWinSize < congestWinSize) {
        maxsend = outWinSize;
    }

    if (outSeqno < outAcked) {
        inTransit = (TCP_MAX_SEQ - outAcked) + outSeqno;
    } else {
        inTransit = outSeqno - outAcked;
    }

    if (maxsend > inTransit) {
        maxsend -= inTransit;
    } else {
        maxsend = 0;
    }

    int sent = 0;
    while ((inQueue.size() > 0) && (maxsend >= MAX_SEG)) {
        dataBuffer *db = inQueue.front();
        inQueue.pop_front();

        TcpPacket *pkt = new TcpPacket(db->data, MAX_SEG);
        sent++;
        maxsend -= MAX_SEG;
        toSend(pkt);
        delete db;
    }

    /* if inqueue empty, and enough window space, send partial stuff */
    if ((!sent) && (inQueue.size() == 0) && (maxsend >= inSize) && (inSize)) {
        TcpPacket *pkt = new TcpPacket(inData, inSize);
        inSize = 0;
        sent++;
        maxsend -= inSize;
        toSend(pkt);
    }

    /* if send nothing */
    bool needsAck = false;

    if (!sent) {
        double cts = getCurrentTS();
        /* if needs ack */
        if (isOldSequence(lastSentAck,inAckno)) {
            needsAck = true;
        }

        /* if needs window
         * if added enough space for packet, or
         * (this case is equivalent to persistence timer)
         * haven't sent anything for a while, and the
         * window size has drastically increased.
         * */
        if (((lastSentWinSize < MAX_SEG) && (inWinSize > MAX_SEG)) ||
                ((cts - keepAliveTimer > retransTimeout * 4) &&
                 (inWinSize > lastSentWinSize + 4 * MAX_SEG))) {
            needsAck = true;
        }

        /* if needs keepalive */
        if (cts - keepAliveTimer > keepAliveTimeout) {
            needsAck = true;
        }


        /* if end of stream->switch mode->send fin (with ack) */
        if ((!outStreamActive) && (inQueue.size() + inSize == 0) &&
                ((state == TCP_ESTABLISHED) || (state == TCP_CLOSE_WAIT))) {
            /* finish the stream */
            TcpPacket *pkt = new TcpPacket();
            pkt->setFin();

            needsAck = false;
            toSend(pkt, false);

            if (state == TCP_ESTABLISHED) {
                state = TCP_FIN_WAIT_1;
                log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::send state => TCP_FIN_WAIT_1");
            } else if (state == TCP_CLOSE_WAIT) {
                state = TCP_LAST_ACK;
                log(LOG_DEBUG_BASIC, TCP_STREAM_ZONE, "TcpStream::send state => TCP_LAST_ACK");
            }

        }

        if (needsAck) {
            sendAck();
        }
    }
    return 1;
}


uint32 TcpStream::genSequenceNo() {
    //return 1000; // TCP_MAX_SEQ - 1000; //1000; //(qrand() - 100000) + time(NULL) % 100000;
    return (qrand() - 100000) + time(NULL) % 100000;
}


bool TcpStream::isOldSequence(uint32 tst, uint32 curr) {
    return ((int)((tst)-(curr)) < 0);

    std::cerr << "TcpStream::isOldSequence(): Case ";
    /* if tst < curr */
    if ((int)((tst)-(curr)) < 0) {
        if (curr - tst < TCP_MAX_SEQ/2) { /* diff less than half span->old */
            std::cerr << "1T" << std::endl;
            return true;
        }
        std::cerr << "2F" << std::endl;
        return false;
    } else if ((tst - curr) > TCP_MAX_SEQ/2) {
        std::cerr << "3T: tst-curr:" << (tst-curr) << std::endl;
        return true;
    }
    std::cerr << "4F: tst-curr:" << (tst-curr) << std::endl;
    return false;
}

#ifdef WINDOWS_SYS
#include <time.h>
#include <sys/timeb.h>
#endif

// Little fn to get current timestamp in an independent manner.
static double getCurrentTS() {

#ifndef WINDOWS_SYS
    struct timeval cts_tmp;
    gettimeofday(&cts_tmp, NULL);
    double cts =  (cts_tmp.tv_sec) + ((double) cts_tmp.tv_usec) / 1000000.0;
#else
    struct _timeb timebuf;
    _ftime( &timebuf);
    double cts =  (timebuf.time) + ((double) timebuf.millitm) / 1000.0;
#endif
    return cts;
}



uint32 TcpStream::int_wbytes() {
    return outSeqno - initOurSeqno - 1;
}

uint32 TcpStream::int_rbytes() {
    return inAckno - initPeerSeqno - 1;
}




/********* Special debugging stuff *****/

#ifdef DEBUG_TCP_STREAM_EXTRA

#include <stdio.h>

static FILE *bc_fd = 0;
int setupBinaryCheck(std::string fname) {
    bc_fd = fopen(fname.c_str(), "r");
    return 1;
}

/* uses seq number to track position->ensure no rollover */
int checkData(uint8 *data, int size, int idx) {
    if (bc_fd <= 0) {
        return -1;
    }
    std::cerr << "checkData(" << idx << "+" << size << ")";

    int  tmpsize = size;
    uint8 tmpdata[tmpsize];
    if (-1 == fseek(bc_fd, idx, SEEK_SET)) {
        std::cerr << "Fseek Issues!" << std::endl;
        exit(1);
        return -1;
    }

    if (1 != fread(tmpdata, tmpsize, 1, bc_fd)) {
        std::cerr << "Length Difference!" << std::endl;
        exit(1);
        return -1;
    }

    for (int i = 0; i < size; i++) {
        if (data[i] != tmpdata[i]) {
            std::cerr << "Byte Difference!" << std::endl;
            exit(1);
            return -1;
        }
    }
    std::cerr << "OK" << std::endl;
    return 1;
}

#endif
