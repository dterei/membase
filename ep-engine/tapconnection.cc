/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 NorthScale, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"
#include "ep_engine.h"
#include "dispatcher.hh"

static void notifyReplicatedItems(std::list<TapLogElement>::iterator from,
                                  std::list<TapLogElement>::iterator to,
                                  EventuallyPersistentEngine &engine);

Atomic<uint64_t> TapConnection::tapCounter(1);

const void *TapConnection::getCookie() const {
    return cookie;
}

void TapConnection::releaseReference(bool force)
{
    if (force || reserved) {
        engine.getServerApi()->cookie->release(cookie);
        setReserved(false);
    }
}


size_t TapProducer::bgMaxPending = 500;
uint32_t TapProducer::ackWindowSize = 10;
uint32_t TapProducer::ackInterval = 1000;
rel_time_t TapProducer::ackGracePeriod = 5 * 60;
double TapProducer::backoffSleepTime = 5.0;
uint32_t TapProducer::initialAckSequenceNumber = 1;
double TapProducer::requeueSleepTime = 0.1;

TapProducer::TapProducer(EventuallyPersistentEngine &theEngine,
                         const void *c,
                         const std::string &n,
                         uint32_t f):
    TapConnection(theEngine, c, n),
    queue(NULL),
    queueSize(0),
    flags(f),
    recordsFetched(0),
    pendingFlush(false),
    reconnects(0),
    paused(false),
    backfillAge(0),
    dumpQueue(false),
    doTakeOver(false),
    takeOverCompletionPhase(false),
    doRunBackfill(false),
    backfillCompleted(true),
    pendingBackfillCounter(0),
    diskBackfillCounter(0),
    vbucketFilter(),
    vBucketHighPriority(),
    vBucketLowPriority(),
    queueMemSize(0),
    queueFill(0),
    queueDrain(0),
    seqno(initialAckSequenceNumber),
    seqnoReceived(initialAckSequenceNumber - 1),
    notifySent(false),
    registeredTAPClient(false),
    isLastAckSucceed(false),
    isSeqNumRotated(false),
    numNoops(0)
{
    evaluateFlags();
    queue = new std::list<queued_item>;

    if (supportAck) {
        expiryTime = ep_current_time() + ackGracePeriod;
    }

    if (cookie != NULL) {
        setReserved(true);
    }
}

void TapProducer::evaluateFlags()
{
    std::stringstream ss;

    if (flags & TAP_CONNECT_FLAG_DUMP) {
        dumpQueue = true;
        ss << ",dump";
    }

    if (flags & TAP_CONNECT_SUPPORT_ACK) {
        TapVBucketEvent hi(TAP_OPAQUE, 0, (vbucket_state_t)htonl(TAP_OPAQUE_ENABLE_AUTO_NACK));
        addVBucketHighPriority(hi);
        supportAck = true;
        ss << ",ack";
    }

    if (flags & TAP_CONNECT_FLAG_BACKFILL) {
        ss << ",backfill";
    }

    if (flags & TAP_CONNECT_FLAG_LIST_VBUCKETS) {
        ss << ",vblist";
    }

    if (flags & TAP_CONNECT_FLAG_TAKEOVER_VBUCKETS) {
        ss << ",takeover";
    }

    if (flags & TAP_CONNECT_CHECKPOINT) {
        TapVBucketEvent event(TAP_OPAQUE, 0,
                              (vbucket_state_t)htonl(TAP_OPAQUE_ENABLE_CHECKPOINT_SYNC));
        addVBucketHighPriority(event);
        supportCheckpointSync = true;
        ss << ",checkpoints";
    }

    if (ss.str().length() > 0) {
        std::stringstream m;
        m.setf(std::ios::hex);
        m << flags << " (" << ss.str().substr(1) << ")";
        flagsText.assign(m.str());
    }
}

void TapProducer::setBackfillAge(uint64_t age, bool reconnect) {
    if (reconnect) {
        if (!(flags & TAP_CONNECT_FLAG_BACKFILL)) {
            age = backfillAge;
        }

        if (age == backfillAge) {
            // we didn't change the critera...
            return;
        }
    }

    if (flags & TAP_CONNECT_FLAG_BACKFILL) {
        backfillAge = age;
    }
}

void TapProducer::setVBucketFilter(const std::vector<uint16_t> &vbuckets)
{
    LockHolder lh(queueLock);
    VBucketFilter diff;

    // time to join the filters..
    if (flags & TAP_CONNECT_FLAG_LIST_VBUCKETS) {
        VBucketFilter filter(vbuckets);
        diff = vbucketFilter.filter_diff(filter);

        const std::vector<uint16_t> &vec = diff.getVector();
        const VBucketMap &vbMap = engine.getEpStore()->getVBuckets();
        // Remove TAP cursors from the vbuckets that don't belong to the new vbucket filter.
        for (std::vector<uint16_t>::const_iterator it = vec.begin(); it != vec.end(); ++it) {
            if (vbucketFilter(*it)) {
                RCPtr<VBucket> vb = vbMap.getBucket(*it);
                if (!vb) {
                    continue;
                }
                vb->checkpointManager.removeTAPCursor(name);
            }
        }

        std::stringstream ss;
        ss << getName().c_str() << ": Changing the vbucket filter from "
           << vbucketFilter << " to "
           << filter << " (diff: " << diff << ")" << std::endl;
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         ss.str().c_str());
        vbucketFilter = filter;

        std::stringstream f;
        f << vbucketFilter;
        filterText.assign(f.str());
    }

    // Note that we do re-evaluete all entries when we suck them out of the
    // queue to send them..
    if (flags & TAP_CONNECT_FLAG_TAKEOVER_VBUCKETS) {
        std::list<TapVBucketEvent> nonVBucketOpaqueMessages;
        std::list<TapVBucketEvent> vBucketOpaqueMessages;
        // Clear vbucket state change messages with a higher priority.
        while (!vBucketHighPriority.empty()) {
            TapVBucketEvent msg = vBucketHighPriority.front();
            vBucketHighPriority.pop();
            if (msg.event == TAP_OPAQUE) {
                uint32_t opaqueCode = (uint32_t) msg.state;
                if (opaqueCode == htonl(TAP_OPAQUE_ENABLE_AUTO_NACK) ||
                    opaqueCode == htonl(TAP_OPAQUE_ENABLE_CHECKPOINT_SYNC)) {
                    nonVBucketOpaqueMessages.push_back(msg);
                } else {
                    vBucketOpaqueMessages.push_back(msg);
                }
            }
        }

        // Add non-vbucket opaque messages back to the high priority queue.
        std::list<TapVBucketEvent>::iterator iter = nonVBucketOpaqueMessages.begin();
        while (iter != nonVBucketOpaqueMessages.end()) {
            addVBucketHighPriority_UNLOCKED(*iter);
            ++iter;
        }

        // Clear vbucket state changes messages with a lower priority.
        while (!vBucketLowPriority.empty()) {
            vBucketLowPriority.pop();
        }

        // Add new vbucket state change messages with a higher or lower priority.
        const std::vector<uint16_t> &vec = vbucketFilter.getVector();
        for (std::vector<uint16_t>::const_iterator it = vec.begin();
             it != vec.end(); ++it) {
            TapVBucketEvent hi(TAP_VBUCKET_SET, *it, vbucket_state_pending);
            TapVBucketEvent lo(TAP_VBUCKET_SET, *it, vbucket_state_active);
            addVBucketHighPriority_UNLOCKED(hi);
            addVBucketLowPriority_UNLOCKED(lo);
        }

        // Add vbucket opaque messages back to the high priority queue.
        iter = vBucketOpaqueMessages.begin();
        while (iter != vBucketOpaqueMessages.end()) {
            addVBucketHighPriority_UNLOCKED(*iter);
            ++iter;
        }
        doTakeOver = true;
    }
}

void TapProducer::registerTAPCursor(std::map<uint16_t, uint64_t> &lastCheckpointIds) {
    LockHolder lh(queueLock);

    tapCheckpointState.clear();
    uint64_t current_time = (uint64_t)ep_real_time();
    std::vector<uint16_t> backfill_vbuckets;
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    size_t numOfVBuckets = vbuckets.getSize();
    for (size_t i = 0; i < numOfVBuckets; ++i) {
        assert(i <= std::numeric_limits<uint16_t>::max());
        uint16_t vbid = static_cast<uint16_t>(i);
        if (vbucketFilter(vbid)) {
            RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
            if (!vb) {
                getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                                 "VBucket %d not found for TAP cursor. Skip it...\n", vbid);
                continue;
            }

            std::map<uint16_t, uint64_t>::iterator it = lastCheckpointIds.find(vbid);
            if (it != lastCheckpointIds.end()) {
                // Now, we assume that the checkpoint Id for a given vbucket is monotonically
                // increased. TODO: If the server supports collapsing multiple closed referenced
                // checkpoints due to very slow TAP clients, the server should maintain the list
                // of checkpoint Ids for each collapsed checkpoint.
                TapCheckpointState st(vbid, it->second + 1, checkpoint_start);
                tapCheckpointState[vbid] = st;
            } else {
                // If a TAP client doesn't specify the last closed checkpoint Id for a given vbucket,
                // check if the checkpoint manager currently has the cursor for that TAP client.
                uint64_t cid = vb->checkpointManager.getCheckpointIdForTAPCursor(name);
                if (cid > 0) {
                    TapCheckpointState st(vbid, cid, checkpoint_start);
                    tapCheckpointState[vbid] = st;
                } else {
                    // Start with the checkpoint 1
                    TapCheckpointState st(vbid, 1, checkpoint_start);
                    tapCheckpointState[vbid] = st;
                }
            }

            // If backfill is currently running for this vbucket, skip the cursor registration.
            if (backfillVBuckets.find(vbid) != backfillVBuckets.end()) {
                TapCheckpointState st(vbid, 0, backfill);
                tapCheckpointState[vbid] = st;
                continue;
            }

            // As TAP dump option simply requires the snapshot of each vbucket, simply schedule
            // backfill and skip the checkpoint cursor registration.
            if (dumpQueue) {
                if (vb->getState() == vbucket_state_active) {
                    backfill_vbuckets.push_back(vbid);
                }
                continue;
            }

            // If the connection is for a registered TAP client that is only interested in closed
            // checkpoints, we always start from the beginning of the checkpoint to which the
            // registered TAP client's cursor currently belongs.
            bool fromBeginning = registeredTAPClient && closedCheckpointOnly;
            // Check if the unified queue contains the checkpoint to start with.
            if(vb && !vb->checkpointManager.registerTAPCursor(name,
                                                       tapCheckpointState[vbid].currentCheckpointId,
                                                       closedCheckpointOnly, fromBeginning)) {
                if (backfillAge < current_time && !registeredTAPClient) { // Backfill is required.
                    TapCheckpointState st(vbid, 0, backfill);
                    tapCheckpointState[vbid] = st;
                    // As we set the cursor to the beginning of the open checkpoint when backfill
                    // is scheduled, we can simply remove the cursor now.
                    vb->checkpointManager.removeTAPCursor(name);
                    if (vb->checkpointManager.getOpenCheckpointId() > 0) {
                        // If the current open checkpoint is 0, it means that this vbucket is still
                        // receiving backfill items from another node. Once the backfill is done,
                        // we will schedule the backfill for this tap connection separately.
                        backfill_vbuckets.push_back(vbid);
                    }
                } else { // If backfill is not required, simply start from the first checkpoint.
                    uint64_t cid = vb->checkpointManager.getCheckpointIdForTAPCursor(name);
                    TapCheckpointState st(vbid, cid, checkpoint_start);
                    tapCheckpointState[vbid] = st;
                }
            }
        }
    }

    if (backfill_vbuckets.size() > 0 && !registeredTAPClient) {
        if (backfillAge < current_time) {
            scheduleBackfill_UNLOCKED(backfill_vbuckets);
        }
    } else {
        doRunBackfill = false;
    }
}

bool TapProducer::windowIsFull() {
    if (!supportAck) {
        return false;
    }

    if (seqno >= seqnoReceived) {
        if ((seqno - seqnoReceived) <= (ackWindowSize * ackInterval)) {
            return false;
        }
    } else {
        uint32_t n = static_cast<uint32_t>(-1) - seqnoReceived + seqno;
        if (n <= (ackWindowSize * ackInterval)) {
            return false;
        }
    }

    return true;
}

bool TapProducer::requestAck(tap_event_t event, uint16_t vbucket) {
    LockHolder lh(queueLock);

    if (!supportAck) {
        // If backfill was scheduled before, check if the backfill is completed or not.
        checkBackfillCompletion_UNLOCKED();
        return false;
    }

    bool isExplicitAck = false;
    if (supportCheckpointSync && (event == TAP_MUTATION || event == TAP_DELETION)) {
        std::map<uint16_t, TapCheckpointState>::iterator map_it =
            tapCheckpointState.find(vbucket);
        if (map_it != tapCheckpointState.end()) {
            map_it->second.lastSeqNum = seqno;
            if (map_it->second.lastItem || map_it->second.state == checkpoint_end) {
                // Always ack for the last item or any items that were NAcked after the cursor
                // reaches to the checkpoint end.
                isExplicitAck = true;
            }
        }
    }

    ++seqno;
    if (seqno == 0) {
        isSeqNumRotated = true;
        seqno = 1;
    }

    bool explicitEvent = false;
    if (event == TAP_VBUCKET_SET ||
        event == TAP_OPAQUE ||
        event == TAP_CHECKPOINT_START ||
        event == TAP_CHECKPOINT_END) {
        explicitEvent = true;
    }

    return (explicitEvent ||
            ((seqno - 1) % ackInterval) == 0 || // ack at a regular interval
            isExplicitAck ||
            (!backfillCompleted && bgResultSize == 0 && queueSize == 0) || // Backfill being done
            empty_UNLOCKED()); // but if we're almost up to date, ack more often
}

void TapProducer::rollback() {
    LockHolder lh(queueLock);
    size_t checkpoint_msg_sent = 0;
    std::vector<uint16_t> backfillVBs;
    std::list<TapLogElement>::iterator i = tapLog.begin();
    while (i != tapLog.end()) {
        switch (i->event) {
        case TAP_VBUCKET_SET:
            {
                TapVBucketEvent e(i->event, i->vbucket, i->state);
                if (i->state == vbucket_state_pending) {
                    addVBucketHighPriority_UNLOCKED(e);
                } else {
                    addVBucketLowPriority_UNLOCKED(e);
                }
            }
            break;
        case TAP_CHECKPOINT_START:
        case TAP_CHECKPOINT_END:
            ++checkpoint_msg_sent;
            addCheckpointMessage_UNLOCKED(i->item);
            break;
        case TAP_FLUSH:
            addEvent_UNLOCKED(i->item);
            break;
        case TAP_DELETION:
        case TAP_MUTATION:
            {
                if (supportCheckpointSync) {
                    std::map<uint16_t, TapCheckpointState>::iterator map_it =
                        tapCheckpointState.find(i->vbucket);
                    if (map_it != tapCheckpointState.end()) {
                        map_it->second.lastSeqNum = std::numeric_limits<uint32_t>::max();
                    } else {
                        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                            "TAP Checkpoint State for VBucket %d Not Found", i->vbucket);
                    }
                }
                addEvent_UNLOCKED(i->item);
            }
            break;
        case TAP_OPAQUE:
            {
                uint32_t val = ntohl((uint32_t)i->state);
                switch (val) {
                case TAP_OPAQUE_ENABLE_AUTO_NACK:
                case TAP_OPAQUE_ENABLE_CHECKPOINT_SYNC:
                    break;
                case TAP_OPAQUE_INITIAL_VBUCKET_STREAM:
                    {
                        backfillVBs.push_back(i->vbucket);
                    }
                    break;
                case TAP_OPAQUE_CLOSE_BACKFILL:
                case TAP_OPAQUE_OPEN_CHECKPOINT:
                case TAP_OPAQUE_START_ONLINEUPDATE:
                case TAP_OPAQUE_STOP_ONLINEUPDATE:
                case TAP_OPAQUE_REVERT_ONLINEUPDATE:
                    {
                        TapVBucketEvent e(i->event, i->vbucket, i->state);
                        addVBucketHighPriority_UNLOCKED(e);
                    }
                    break;
                default:
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "Internal error. Not implemented");
                    abort();
                }
            }
            break;
        default:
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Internal error. Not implemented");
            abort();
        }
        tapLog.erase(i);
        i = tapLog.begin();
    }

    if (backfillVBs.size() > 0) {
        scheduleBackfill_UNLOCKED(backfillVBs);
    }
    seqnoReceived = seqno - 1;
    checkpointMsgCounter -= checkpoint_msg_sent;
}

/**
 * Dispatcher task to wake a tap connection.
 */
class TapResumeCallback : public DispatcherCallback {
public:
    TapResumeCallback(EventuallyPersistentEngine &e, TapProducer &c)
        : engine(e), connection(c) {
        std::stringstream ss;
        ss << "Resuming suspended tap connection: " << connection.getName();
        descr = ss.str();
    }

    bool callback(Dispatcher &, TaskId) {
        connection.setSuspended(false);
        // The notify io thread will pick up this connection and resume it
        // Since we was suspended I guess we can wait a little bit
        // longer ;)
        return false;
    }

    std::string description() {
        return descr;
    }

private:
    EventuallyPersistentEngine &engine;
    TapProducer &connection;
    std::string descr;
};

bool TapProducer::isSuspended() const {
    return suspended.get();
}

void TapProducer::setSuspended(bool value)
{
    if (value) {
        if (backoffSleepTime > 0 && !suspended.get()) {
            double sleepTime = takeOverCompletionPhase ? 0.5 : backoffSleepTime;
            Dispatcher *d = engine.getEpStore()->getNonIODispatcher();
            d->schedule(shared_ptr<DispatcherCallback>
                        (new TapResumeCallback(engine, *this)),
                        NULL, Priority::TapResumePriority, sleepTime,
                        false);
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Suspend %s for %.2f secs\n", getName().c_str(),
                             sleepTime);


        } else {
            // backoff disabled, or already in a suspended state
            return;
        }
    }
    suspended.set(value);
}

void TapProducer::reschedule_UNLOCKED(const std::list<TapLogElement>::iterator &iter)
{
    ++numTmpfailSurvivors;
    switch (iter->event) {
    case TAP_VBUCKET_SET:
        {
            TapVBucketEvent e(iter->event, iter->vbucket, iter->state);
            if (iter->state == vbucket_state_pending) {
                addVBucketHighPriority_UNLOCKED(e);
            } else {
                addVBucketLowPriority_UNLOCKED(e);
            }
        }
        break;
    case TAP_CHECKPOINT_START:
    case TAP_CHECKPOINT_END:
        --checkpointMsgCounter;
        addCheckpointMessage_UNLOCKED(iter->item);
        break;
    case TAP_FLUSH:
        addEvent_UNLOCKED(iter->item);
        break;
    case TAP_DELETION:
    case TAP_MUTATION:
        {
            if (supportCheckpointSync) {
                std::map<uint16_t, TapCheckpointState>::iterator map_it =
                    tapCheckpointState.find(iter->vbucket);
                if (map_it != tapCheckpointState.end()) {
                    map_it->second.lastSeqNum = std::numeric_limits<uint32_t>::max();
                }
            }
            addEvent_UNLOCKED(iter->item);
        }
        break;
    case TAP_OPAQUE:
        {
            TapVBucketEvent ev(iter->event, iter->vbucket,
                                         (vbucket_state_t)iter->state);
            addVBucketHighPriority_UNLOCKED(ev);
        }
        break;
    default:
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Internal error. Not implemented");
        abort();
    }
}

ENGINE_ERROR_CODE TapProducer::processAck(uint32_t s,
                                          uint16_t status,
                                          const std::string &msg)
{
    LockHolder lh(queueLock);
    std::list<TapLogElement>::iterator iter = tapLog.begin();
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;

    expiryTime = ep_current_time() + ackGracePeriod;
    if (isSeqNumRotated && s < seqnoReceived) {
        // if the ack seq number is rotated, reset the last seq number of each vbucket to 0.
        std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
        for (; it != tapCheckpointState.end(); ++it) {
            it->second.lastSeqNum = 0;
        }
        isSeqNumRotated = false;
    }
    seqnoReceived = s;
    isLastAckSucceed = false;

    /* Implicit ack _every_ message up until this message */
    while (iter != tapLog.end() && iter->seqno != s) {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Implicit ack <%s> (#%u)\n",
                         getName().c_str(), iter->seqno);
        ++iter;
    }

    bool notifyTapNotificationThread = false;

    switch (status) {
    case PROTOCOL_BINARY_RESPONSE_SUCCESS:
        /* And explicit ack this message! */
        if (iter != tapLog.end()) {
            // If this ACK is for TAP_CHECKPOINT messages, indicate that the checkpoint
            // is synced between the master and slave nodes.
            if ((iter->event == TAP_CHECKPOINT_START || iter->event == TAP_CHECKPOINT_END)
                && supportCheckpointSync) {
                std::map<uint16_t, TapCheckpointState>::iterator map_it =
                    tapCheckpointState.find(iter->vbucket);
                if (iter->event == TAP_CHECKPOINT_END && map_it != tapCheckpointState.end()) {
                    map_it->second.state = checkpoint_end_synced;
                }
                --checkpointMsgCounter;
                notifyTapNotificationThread = true;
            }
            getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                             "Explicit ack <%s> (#%u)\n",
                             getName().c_str(), iter->seqno);
            ++iter;
            notifyReplicatedItems(tapLog.begin(), iter, engine);
            tapLog.erase(tapLog.begin(), iter);
            isLastAckSucceed = true;
        } else {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Explicit ack <%s> of nonexisting entry (#%u)\n",
                             getName().c_str(), s);
        }

        if (checkBackfillCompletion_UNLOCKED()) {
            notifyTapNotificationThread = true;
        }

        lh.unlock();

        if (notifyTapNotificationThread || doTakeOver) {
            engine.notifyTapNotificationThread();
        }

        if (complete() && idle()) {
            // We've got all of the ack's need, now we can shut down the
            // stream
            setDisconnect(true);
            expiryTime = 0;
            ret = ENGINE_DISCONNECT;
        }
        break;

    case PROTOCOL_BINARY_RESPONSE_EBUSY:
    case PROTOCOL_BINARY_RESPONSE_ETMPFAIL:
        setSuspended(true);
        ++numTapNack;
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Received temporary TAP nack from <%s> (#%u): Code: %u (%s)\n",
                         getName().c_str(), seqnoReceived, status, msg.c_str());

        notifyReplicatedItems(tapLog.begin(), iter, engine);
        // Reschedule _this_ sequence number..
        if (iter != tapLog.end()) {
            reschedule_UNLOCKED(iter);
            ++iter;
        }
        tapLog.erase(tapLog.begin(), iter);
        break;
    default:
        notifyReplicatedItems(tapLog.begin(), iter, engine);
        tapLog.erase(tapLog.begin(), iter);
        ++numTapNack;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Received negative TAP ack from <%s> (#%u): Code: %u (%s)\n",
                         getName().c_str(), seqnoReceived, status, msg.c_str());
        setDisconnect(true);
        expiryTime = 0;
        ret = ENGINE_DISCONNECT;
    }

    return ret;
}

bool TapProducer::checkBackfillCompletion_UNLOCKED() {
    bool rv = false;
    if (!backfillCompleted && !isPendingBackfill_UNLOCKED() &&
        getBackfillRemaining_UNLOCKED() == 0 && tapLog.empty()) {

        backfillCompleted = true;
        std::set<uint16_t>::iterator it = backfillVBuckets.begin();
        for (; it != backfillVBuckets.end(); ++it) {
            TapVBucketEvent backfillEnd(TAP_OPAQUE, *it,
                                        (vbucket_state_t)htonl(TAP_OPAQUE_CLOSE_BACKFILL));
            addVBucketHighPriority_UNLOCKED(backfillEnd);
        }
        backfillVBuckets.clear();

        rv = true;
    }
    return rv;
}

void TapProducer::encodeVBucketStateTransition(const TapVBucketEvent &ev, void **es,
                                                 uint16_t *nes, uint16_t *vbucket) const
{
    *vbucket = ev.vbucket;
    switch (ev.state) {
    case vbucket_state_active:
        *es = const_cast<void*>(static_cast<const void*>(&VBucket::ACTIVE));
        break;
    case vbucket_state_replica:
        *es = const_cast<void*>(static_cast<const void*>(&VBucket::REPLICA));
        break;
    case vbucket_state_pending:
        *es = const_cast<void*>(static_cast<const void*>(&VBucket::PENDING));
        break;
    case vbucket_state_dead:
        *es = const_cast<void*>(static_cast<const void*>(&VBucket::DEAD));
        break;
    default:
        // Illegal vbucket state
        abort();
    }
    *nes = sizeof(vbucket_state_t);
}

bool TapProducer::waitForBackfill() {
    LockHolder lh(queueLock);
    if ((bgJobIssued - bgJobCompleted) > bgMaxPending) {
        return true;
    }
    return false;
}

bool TapProducer::waitForCheckpointMsgAck() {
    return checkpointMsgCounter > 0;
}

/**
 * A Dispatcher job that performs a background fetch on behalf of tap.
 */
class TapBGFetchCallback : public DispatcherCallback {
public:
    TapBGFetchCallback(EventuallyPersistentEngine *e, const std::string &n,
                       const std::string &k, uint16_t vbid, uint16_t vbv,
                       uint64_t r, const void *c) :
        epe(e), name(n), key(k), vbucket(vbid), vbver(vbv), rowid(r), cookie(c),
        init(gethrtime()), start(0), counter(e->getEpStore()->bgFetchQueue) {
        assert(epe);
        assert(cookie);
    }

    bool callback(Dispatcher & d, TaskId t) {
        start = gethrtime();
        RememberingCallback<GetValue> gcb;

        EPStats &stats = epe->getEpStats();
        EventuallyPersistentStore *epstore = epe->getEpStore();
        assert(epstore);

        epstore->getROUnderlying()->get(key, rowid, vbucket, vbver, gcb);
        gcb.waitForValue();
        assert(gcb.fired);

        if (gcb.val.getStatus() == ENGINE_SUCCESS) {
            ReceivedItemTapOperation tapop;
            // if the tap connection is closed, then free an Item instance
            if (!epe->tapConnMap.performTapOp(name, tapop, gcb.val.getValue())) {
                delete gcb.val.getValue();
            }
            epe->notifyIOComplete(cookie, ENGINE_SUCCESS);
        } else { // If a tap bg fetch job is failed, schedule it again.
            RCPtr<VBucket> vb = epstore->getVBucket(vbucket);
            if (vb) {
                int bucket_num(0);
                LockHolder lh = vb->ht.getLockedBucket(key, &bucket_num);
                StoredValue *v = epstore->fetchValidValue(vb, key, bucket_num);
                if (v) {
                    d.snooze(t, TapProducer::requeueSleepTime);
                    ++stats.numTapBGFetchRequeued;
                    return true;
                }
            }
        }

        CompletedBGFetchTapOperation tapop;
        epe->tapConnMap.performTapOp(name, tapop, epe);

        hrtime_t stop = gethrtime();

        if (stop > start && start > init) {
            // skip the measurement if the counter wrapped...
            ++stats.tapBgNumOperations;
            hrtime_t w = (start - init) / 1000;
            stats.tapBgWait += w;
            stats.tapBgWaitHisto.add(w);
            stats.tapBgMinWait.setIfLess(w);
            stats.tapBgMaxWait.setIfBigger(w);

            hrtime_t l = (stop - start) / 1000;
            stats.tapBgLoad += l;
            stats.tapBgLoadHisto.add(l);
            stats.tapBgMinLoad.setIfLess(l);
            stats.tapBgMaxLoad.setIfBigger(l);
        }

        return false;
    }

    std::string description() {
        std::stringstream ss;
        ss << "Fetching item from disk for tap:  " << key;
        return ss.str();
    }

private:
    EventuallyPersistentEngine *epe;
    const std::string           name;
    std::string                 key;
    uint16_t                    vbucket;
    uint16_t                    vbver;
    uint64_t                    rowid;
    const void                 *cookie;

    hrtime_t init;
    hrtime_t start;

    BGFetchCounter counter;
};

void TapProducer::queueBGFetch(const std::string &key, uint64_t id,
                                 uint16_t vb, uint16_t vbv) {
    LockHolder lh(queueLock);
    backfillQueue.push(TapBGFetchQueueItem(key, id, vb, vbv));
    ++bgQueued;
    ++bgQueueSize;
    assert(!empty_UNLOCKED());
    assert(!idle_UNLOCKED());
    assert(!complete_UNLOCKED());
}

void TapProducer::runBGFetch(Dispatcher *dispatcher, const void *c) {
    LockHolder lh(queueLock);
    TapBGFetchQueueItem qi(backfillQueue.front());
    backfillQueue.pop();
    --bgQueueSize;

    shared_ptr<TapBGFetchCallback> dcb(new TapBGFetchCallback(&engine,
                                                              getName(), qi.key,
                                                              qi.vbucket, qi.vbversion,
                                                              qi.id, c));
    ++bgJobIssued;
    dispatcher->schedule(dcb, NULL, Priority::TapBgFetcherPriority);
}

void TapProducer::gotBGItem(Item *i, bool implicitEnqueue) {
    LockHolder lh(queueLock);
    // implicitEnqueue is used for the optimized disk fetch wherein we
    // receive the item and want the stats to reflect an
    // enqueue/execute cycle.
    if (implicitEnqueue) {
        ++bgQueued;
        ++bgJobIssued;
        ++bgJobCompleted;
    }
    backfilledItems.push(i);
    ++bgResultSize;
    assert(hasItem());
}

void TapProducer::completedBGFetchJob() {
    ++bgJobCompleted;
}

Item* TapProducer::nextFetchedItem() {
    LockHolder lh(queueLock);
    assert(hasItem());
    Item *rv = backfilledItems.front();
    assert(rv);
    backfilledItems.pop();
    --bgResultSize;
    return rv;
}

void TapProducer::addStats(ADD_STAT add_stat, const void *c) {
    TapConnection::addStats(add_stat, c);
    addStat("qlen", getQueueSize(), add_stat, c);
    addStat("qlen_high_pri", vBucketHighPriority.size(), add_stat, c);
    addStat("qlen_low_pri", vBucketLowPriority.size(), add_stat, c);
    addStat("vb_filters", vbucketFilter.size(), add_stat, c);
    addStat("vb_filter", filterText.c_str(), add_stat, c);
    addStat("rec_fetched", recordsFetched, add_stat, c);
    if (recordsSkipped > 0) {
        addStat("rec_skipped", recordsSkipped, add_stat, c);
    }
    addStat("idle", idle(), add_stat, c);
    addStat("empty", empty(), add_stat, c);
    addStat("complete", complete(), add_stat, c);
    addStat("has_item", hasItem(), add_stat, c);
    addStat("has_queued_item", hasQueuedItem(), add_stat, c);
    addStat("bg_wait_for_results", waitForBackfill(), add_stat, c);
    addStat("bg_queue_size", bgQueueSize, add_stat, c);
    addStat("bg_queued", bgQueued, add_stat, c);
    addStat("bg_result_size", bgResultSize, add_stat, c);
    addStat("bg_results", bgResults, add_stat, c);
    addStat("bg_jobs_issued", bgJobIssued, add_stat, c);
    addStat("bg_jobs_completed", bgJobCompleted, add_stat, c);
    addStat("bg_backlog_size", getRemaingOnDisk(), add_stat, c);
    addStat("flags", flagsText, add_stat, c);
    addStat("suspended", isSuspended(), add_stat, c);
    addStat("paused", paused, add_stat, c);
    addStat("pending_backfill", isPendingBackfill(), add_stat, c);
    addStat("pending_disk_backfill", isPendingDiskBackfill(), add_stat, c);
    addStat("backfill_completed", isBackfillCompleted(), add_stat, c);

    addStat("queue_memory", getQueueMemory(), add_stat, c);
    addStat("queue_fill", getQueueFillTotal(), add_stat, c);
    addStat("queue_drain", getQueueDrainTotal(), add_stat, c);
    addStat("queue_backoff", getQueueBackoff(), add_stat, c);
    addStat("queue_backfillremaining", getBackfillRemaining(), add_stat, c);
    addStat("queue_itemondisk", getRemaingOnDisk(), add_stat, c);
    addStat("total_backlog_size", getBackfillRemaining() + getRemainingOnCheckpoints(),
            add_stat, c);
    addStat("total_noops", numNoops, add_stat, c);

    if (reconnects > 0) {
        addStat("reconnects", reconnects, add_stat, c);
    }
    if (backfillAge != 0) {
        addStat("backfill_age", (size_t)backfillAge, add_stat, c);
    }

    if (supportAck) {
        addStat("ack_seqno", seqno, add_stat, c);
        addStat("recv_ack_seqno", seqnoReceived, add_stat, c);
        addStat("ack_log_size", getTapAckLogSize(), add_stat, c);
        addStat("ack_window_full", windowIsFull(), add_stat, c);
        if (windowIsFull()) {
            addStat("expires", expiryTime - ep_current_time(), add_stat, c);
        }
        addStat("num_tap_nack", numTapNack, add_stat, c);
        addStat("num_tap_tmpfail_survivors", numTmpfailSurvivors, add_stat, c);
        addStat("ack_playback_size", getTapAckLogSize(), add_stat, c);
     }
}

void TapProducer::processedEvent(tap_event_t event, ENGINE_ERROR_CODE)
{
    assert(event == TAP_ACK);
}

/**************** TAP Consumer **********************************************/
TapConsumer::TapConsumer(EventuallyPersistentEngine &theEngine,
                         const void *c,
                         const std::string &n) :
    TapConnection(theEngine, c, n)
{ /* EMPTY */ }

void TapConsumer::addStats(ADD_STAT add_stat, const void *c) {
    TapConnection::addStats(add_stat, c);
    addStat("num_delete", numDelete, add_stat, c);
    addStat("num_delete_failed", numDeleteFailed, add_stat, c);
    addStat("num_flush", numFlush, add_stat, c);
    addStat("num_flush_failed", numFlushFailed, add_stat, c);
    addStat("num_mutation", numMutation, add_stat, c);
    addStat("num_mutation_failed", numMutationFailed, add_stat, c);
    addStat("num_opaque", numOpaque, add_stat, c);
    addStat("num_opaque_failed", numOpaqueFailed, add_stat, c);
    addStat("num_vbucket_set", numVbucketSet, add_stat, c);
    addStat("num_vbucket_set_failed", numVbucketSetFailed, add_stat, c);
    addStat("num_checkpoint_start", numCheckpointStart, add_stat, c);
    addStat("num_checkpoint_start_failed", numCheckpointStartFailed, add_stat, c);
    addStat("num_checkpoint_end", numCheckpointEnd, add_stat, c);
    addStat("num_checkpoint_end_failed", numCheckpointEndFailed, add_stat, c);
    addStat("num_unknown", numUnknown, add_stat, c);
}

void TapConsumer::setBackfillPhase(bool isBackfill, uint16_t vbucket) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!vb) {
        return;
    }

    vb->setBackfillPhase(isBackfill);
    if (isBackfill) {
        // set the open checkpoint id to 0 to indicate the backfill phase.
        vb->checkpointManager.setOpenCheckpointId(0);
        // Note that when backfill is started, the destination always resets the vbucket
        // and its checkpoint datastructure.
    } else {
        // If backfill is completed for a given vbucket subscribed by this consumer, schedule
        // backfill for all TAP connections that are currently replicating that vbucket,
        // so that replica chain can be synchronized.
        std::set<uint16_t> backfillVB;
        backfillVB.insert(vbucket);
        TapConnMap &connMap = engine.getTapConnMap();
        connMap.scheduleBackfill(backfillVB);
    }
}

bool TapConsumer::isBackfillPhase(uint16_t vbucket) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (vb && vb->isBackfillPhase()) {
        return true;
    }
    return false;
}

void TapConsumer::processedEvent(tap_event_t event, ENGINE_ERROR_CODE ret)
{
    switch (event) {
    case TAP_ACK:
        /* A tap consumer should _NEVER_ receive a tap ack */
        abort();
        break;

    case TAP_FLUSH:
        if (ret == ENGINE_SUCCESS) {
            ++numFlush;
        } else {
            ++numFlushFailed;
        }
        break;

    case TAP_DELETION:
        if (ret == ENGINE_SUCCESS) {
            ++numDelete;
        } else {
            ++numDeleteFailed;
        }
        break;

    case TAP_MUTATION:
        if (ret == ENGINE_SUCCESS) {
            ++numMutation;
        } else {
            ++numMutationFailed;
        }
        break;

    case TAP_OPAQUE:
        if (ret == ENGINE_SUCCESS) {
            ++numOpaque;
        } else {
            ++numOpaqueFailed;
        }
        break;

    case TAP_VBUCKET_SET:
        if (ret == ENGINE_SUCCESS) {
            ++numVbucketSet;
        } else {
            ++numVbucketSetFailed;
        }
        break;

    case TAP_CHECKPOINT_START:
        if (ret == ENGINE_SUCCESS) {
            ++numCheckpointStart;
        } else {
            ++numCheckpointStartFailed;
        }
        break;

    case TAP_CHECKPOINT_END:
        if (ret == ENGINE_SUCCESS) {
            ++numCheckpointEnd;
        } else {
            ++numCheckpointEndFailed;
        }
        break;

    default:
        ++numUnknown;
    }
}

bool TapConsumer::processCheckpointCommand(tap_event_t event, uint16_t vbucket,
                                           uint64_t checkpointId) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!vb) {
        return false;
    }

    // If the vbucket is in active, but not allowed to accept checkpoint messaages, simply ignore
    // those messages.
    if (vb->getState() == vbucket_state_active &&
        !CheckpointManager::inconsistentSlaveCheckpoint) {
        return true;
    }

    bool ret = true;
    switch (event) {
    case TAP_CHECKPOINT_START:
        {
            // This is necessary for supporting backward compatibility to 1.7
            if (vb->isBackfillPhase() && checkpointId > 0) {
                setBackfillPhase(false, vbucket);
            }

            bool persistenceCursorRepositioned = false;
            ret = vb->checkpointManager.checkAndAddNewCheckpoint(checkpointId,
                                                                 persistenceCursorRepositioned);
            if (ret && persistenceCursorRepositioned) {
                // If persistence cursor is reset to the beginning of the new checkpoint, set
                // the ID of the last persisted checkpoint to (new checkpoint ID -1) if necessary
                engine.getEpStore()->setPersistenceCheckpointId(vbucket, checkpointId - 1);
            }
        }
        break;
    case TAP_CHECKPOINT_END:
        ret = vb->checkpointManager.closeOpenCheckpoint(checkpointId);
        break;
    default:
        ret = false;
        break;
    }
    return ret;
}

void TapConsumer::checkVBOpenCheckpoint(uint16_t vbucket) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!vb) {
        return;
    }
    bool forceCreation = vb->checkpointManager.isCheckpointCreationForHighMemUsage(vb);
    vb->checkpointManager.checkOpenCheckpoint(forceCreation, true);
}

bool TapConsumer::processOnlineUpdateCommand(uint32_t opcode, uint16_t vbucket) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!vb) {
        return false;
    }

    protocol_binary_response_status ret;
    switch (opcode) {
    case TAP_OPAQUE_START_ONLINEUPDATE:
        ret = vb->checkpointManager.startOnlineUpdate();
        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "Start online update\n");
        break;
    case TAP_OPAQUE_STOP_ONLINEUPDATE:
        ret = vb->checkpointManager.stopOnlineUpdate();
        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "Complete online update\n");
        break;
    case TAP_OPAQUE_REVERT_ONLINEUPDATE:
        ret = engine.getEpStore()->revertOnlineUpdate(vb);
        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "Revert online update\n");
        break;
    default:
        ret = PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
        break;
    }
    return (ret == PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

bool TapProducer::isTimeForNoop() {
    bool rv = noop.swap(false);
    if (rv) {
        ++numNoops;
    }
    return rv;
}

void TapProducer::setTimeForNoop()
{
    noop.set(true);
}

bool TapProducer::cleanSome()
{
    int ii = 0;
    LockHolder lh(queueLock);
    while (!backfilledItems.empty() && ii < 1000) {
        Item *i(backfilledItems.front());
        assert(i);
        delete i;
        backfilledItems.pop();
        --bgResultSize;
        ++ii;
    }

    return backfilledItems.empty();
}

queued_item TapProducer::next(bool &shouldPause) {
    LockHolder lh(queueLock);
    shouldPause = false;

    if (queue->empty() && isBackfillCompleted_UNLOCKED()) {
        const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
        uint16_t invalid_count = 0;
        uint16_t open_checkpoint_count = 0;
        uint16_t wait_for_ack_count = 0;

        std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
        for (; it != tapCheckpointState.end(); ++it) {
            uint16_t vbid = it->first;
            RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
            if (!vb || (vb->getState() == vbucket_state_dead && !doTakeOver)) {
                ++invalid_count;
                continue;
            }

            bool isLastItem = false;
            queued_item item = vb->checkpointManager.nextItem(name, isLastItem);
            switch(item->getOperation()) {
            case queue_op_set:
            case queue_op_del:
                if (supportCheckpointSync && isLastItem) {
                    it->second.lastItem = true;
                } else {
                    it->second.lastItem = false;
                }
                addEvent_UNLOCKED(item);
                break;
            case queue_op_checkpoint_start:
                {
                    uint64_t checkpointId;
                    memcpy(&checkpointId, item->getValue()->getData(), sizeof(checkpointId));
                    checkpointId = ntohll(checkpointId);
                    it->second.currentCheckpointId = checkpointId;
                    if (supportCheckpointSync) {
                        it->second.state = checkpoint_start;
                        addCheckpointMessage_UNLOCKED(item);
                    }
                }
                break;
            case queue_op_checkpoint_end:
                if (supportCheckpointSync) {
                    it->second.state = checkpoint_end;
                    uint32_t seqnoAcked;
                    if (seqnoReceived == 0) {
                        seqnoAcked = 0;
                    } else {
                        seqnoAcked = isLastAckSucceed ? seqnoReceived : seqnoReceived - 1;
                    }
                    if (it->second.lastSeqNum <= seqnoAcked) {
                        addCheckpointMessage_UNLOCKED(item);
                    } else {
                        vb->checkpointManager.decrTapCursorFromCheckpointEnd(name);
                        ++wait_for_ack_count;
                    }
                }
                break;
            case queue_op_online_update_start:
                {
                    TapVBucketEvent ev(TAP_OPAQUE, item->getVBucketId(),
                                         (vbucket_state_t)htonl(TAP_OPAQUE_START_ONLINEUPDATE));
                    addVBucketHighPriority_UNLOCKED(ev);
                }
                break;
            case queue_op_online_update_end:
                {
                    TapVBucketEvent ev(TAP_OPAQUE, item->getVBucketId(),
                                         (vbucket_state_t)htonl(TAP_OPAQUE_STOP_ONLINEUPDATE));
                    addVBucketHighPriority_UNLOCKED(ev);
                }
                break;
            case queue_op_online_update_revert:
                {
                    TapVBucketEvent ev(TAP_OPAQUE, item->getVBucketId(),
                                         (vbucket_state_t)htonl(TAP_OPAQUE_REVERT_ONLINEUPDATE));
                    addVBucketHighPriority_UNLOCKED(ev);
                }
                break;
            case queue_op_empty:
                {
                    ++open_checkpoint_count;
                    if (closedCheckpointOnly) {
                        // If all the cursors are at the open checkpoints, send the OPAQUE message
                        // to the TAP client so that it can close the connection if necessary.
                        if (open_checkpoint_count == (tapCheckpointState.size() - invalid_count)) {
                            TapVBucketEvent ev(TAP_OPAQUE, item->getVBucketId(),
                                               (vbucket_state_t)htonl(TAP_OPAQUE_OPEN_CHECKPOINT));
                            addVBucketHighPriority_UNLOCKED(ev);
                        }
                    }
                }
                break;
            default:
                break;
            }
        }

        if (wait_for_ack_count == (tapCheckpointState.size() - invalid_count)) {
            // All the TAP cursors are now at their checkpoint end position and should wait until
            // they are implicitly acked for all items belonging to their corresponding checkpoint.
            shouldPause = true;
        } else if ((wait_for_ack_count + open_checkpoint_count) ==
                   (tapCheckpointState.size() - invalid_count)) {
            // All the TAP cursors are either at their checkpoint end position to wait for acks or
            // reaches to the end of the current open checkpoint.
            shouldPause = true;
        }
    }

    if (!queue->empty()) {
        queued_item qi = queue->front();
        queue->pop_front();
        --queueSize;
        if (queueMemSize > sizeof(queued_item)) {
            queueMemSize.decr(sizeof(queued_item));
        } else {
            queueMemSize.set(0);
        }
        ++recordsFetched;
        return qi;
    }

    if (!isBackfillCompleted_UNLOCKED()) {
        shouldPause = true;
    }
    queued_item empty_item(new QueuedItem("", 0xffff, queue_op_empty));
    return empty_item;
}

size_t TapProducer::getRemainingOnCheckpoints() {
    LockHolder lh(queueLock);

    size_t numItems = 0;
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
    for (; it != tapCheckpointState.end(); ++it) {
        uint16_t vbid = it->first;
        RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
        if (!vb || (vb->getState() == vbucket_state_dead && !doTakeOver)) {
            continue;
        }
        numItems += vb->checkpointManager.getNumItemsForTAPConnection(name);
    }
    return numItems;
}

bool TapProducer::hasNextFromCheckpoints_UNLOCKED() {
    bool hasNext = false;
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
    for (; it != tapCheckpointState.end(); ++it) {
        uint16_t vbid = it->first;
        RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
        if (!vb || (vb->getState() == vbucket_state_dead && !doTakeOver)) {
            continue;
        }
        hasNext = vb->checkpointManager.hasNext(name);
        if (hasNext) {
            break;
        }
    }
    return hasNext;
}

bool TapProducer::SetCursorToOpenCheckpoint(uint16_t vbid) {
    LockHolder lh(queueLock);
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    if (!vb || vb->getState() == vbucket_state_dead) {
        return false;
    }

    uint64_t checkpointId = vb->checkpointManager.getOpenCheckpointId();
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.find(vbid);
    if (it == tapCheckpointState.end() || dumpQueue) {
        return false;
    }

    vb->checkpointManager.registerTAPCursor(name, checkpointId, closedCheckpointOnly, true);
    it->second.currentCheckpointId = checkpointId;
    return true;
}

void TapProducer::setRegisteredClient(bool isRegisteredClient) {
    registeredTAPClient = isRegisteredClient;
}

void TapProducer::setClosedCheckpointOnlyFlag(bool isClosedCheckpointOnly) {
    closedCheckpointOnly = isClosedCheckpointOnly;
}

void TapProducer::scheduleBackfill_UNLOCKED(const std::vector<uint16_t> &vblist) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    std::vector<uint16_t> vbs;
    std::vector<uint16_t>::const_iterator vbit = vblist.begin();
    // Skip all the vbuckets that are receiving backfill items from the upstream servers.
    for (; vbit != vblist.end(); ++vbit) {
        RCPtr<VBucket> vb = vbuckets.getBucket(*vbit);
        if (vb && !vb->isBackfillPhase()) {
            vbs.push_back(*vbit);
        }
    }

    if (vbs.empty()) {
        return;
    }

    if (!backfillCompleted) {
        // Backfill tasks from the current backfill session are still running.
        // Simply add the new vbuckets only to the next backfill task.

        if (!doRunBackfill) {
            std::vector<uint16_t> emptyVBs;
            // Clear backfill vb filter for the next backfill task
            backFillVBucketFilter.assign(emptyVBs);
        }
        std::vector<uint16_t>::iterator it = vbs.begin();
        for(; it != vbs.end(); ++it) {
            std::pair<std::set<uint16_t>::iterator, bool> ret = backfillVBuckets.insert(*it);
            if (ret.second) {
                backFillVBucketFilter.addVBucket(*it);
            }
        }
    } else {
        // All backfill tasks from the current backfill session were completed.
        // This will create a new backfill session.
        backfillVBuckets.clear();
        backfillVBuckets.insert(vbs.begin(), vbs.end());
        backFillVBucketFilter.assign(vbs);
    }

    // Send an initial_vbucket_stream message to the destination node so that it can
    // delete the corresponding vbucket before receiving the backfill stream.
    const std::vector<uint16_t> &newBackfillVBs = backFillVBucketFilter.getVector();
    std::vector<uint16_t>::const_iterator it = newBackfillVBs.begin();
    for (; it != newBackfillVBs.end(); ++it) {
        TapVBucketEvent hi(TAP_OPAQUE, *it,
                           (vbucket_state_t)htonl(TAP_OPAQUE_INITIAL_VBUCKET_STREAM));
        addVBucketHighPriority_UNLOCKED(hi);
    }

    if (newBackfillVBs.size() > 0) {
        doRunBackfill = true;
        backfillCompleted = false;
    }
}

static void notifyReplicatedItems(std::list<TapLogElement>::iterator from,
                                  std::list<TapLogElement>::iterator to,
                                  EventuallyPersistentEngine &engine) {

    for (std::list<TapLogElement>::iterator it = from; it != to; ++it) {
        if (it->event == TAP_MUTATION) {
            queued_item qi = it->item;
            StoredValue *sv = engine.getEpStore()->getStoredValue(qi->getKey(),
                                                                  qi->getVBucketId(),
                                                                  false);
            if (sv != NULL) {
                sv->incrementNumReplicas();
                engine.getSyncRegistry().itemReplicated(qi);
            }
        }
    }
}
