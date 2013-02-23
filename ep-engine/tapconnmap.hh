/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef TAPCONNMAP_HH
#define TAPCONNMAP_HH 1

#include <map>
#include <list>
#include <iterator>

#include "common.hh"
#include "queueditem.hh"
#include "locks.hh"
#include "syncobject.hh"

// Forward declaration
class TapConnection;
class TapConsumer;
class TapProducer;
class TapConnection;
class Item;
class EventuallyPersistentEngine;

/**
 * Base class for operations performed on tap connections.
 *
 * @see TapConnMap::performTapOp
 */
template <typename V>
class TapOperation {
public:
    virtual ~TapOperation() {}
    virtual void perform(TapProducer *tc, V arg) = 0;
};

/**
 * Indicate the tap operation is complete.
 */
class CompleteBackfillTapOperation : public TapOperation<void*> {
public:
    void perform(TapProducer *tc, void* arg);
};

/**
 * Indicate that we are going to schedule a tap disk backfill for a given vbucket.
 */
class ScheduleDiskBackfillTapOperation : public TapOperation<void*> {
public:
    void perform(TapProducer *tc, void* arg);
};

/**
 * Indicate the tap backfill disk stream thing is complete for a given vbucket.
 */
class CompleteDiskBackfillTapOperation : public TapOperation<void*> {
public:
    void perform(TapProducer *tc, void* arg);
};

/**
 * Give the tap connection a new item.
 */
class ReceivedItemTapOperation : public TapOperation<Item*> {
public:
    ReceivedItemTapOperation(bool ie=false) : implicitEnqueue(ie) {}

    void perform(TapProducer *tc, Item* arg);
private:
    bool implicitEnqueue;
};

/**
 * Indicate a background fetch completed on a tap connection.
 */
class CompletedBGFetchTapOperation : public TapOperation<EventuallyPersistentEngine*> {
public:
    void perform(TapProducer *tc, EventuallyPersistentEngine* arg);
};

/**
 * The noop tap operation will notify paused tap connections..
 */
class NotifyPausedTapOperation : public TapOperation<EventuallyPersistentEngine*> {
public:
    void perform(TapProducer *, EventuallyPersistentEngine*) {}
};

/**
 * A collection of tap connections.
 */
class TapConnMap {
public:
    TapConnMap(EventuallyPersistentEngine &theEngine) : engine(theEngine) {}


    /**
     * Disconnect a tap connection by its cookie.
     */
    void disconnect(const void *cookie, int tapKeepAlive);

    /**
     * Perform a TapOperation for a named tap connection while holding
     * appropriate locks.
     *
     * @param name the name of the tap connection to run the op
     * @param tapop the operation to perform
     * @param arg argument for the tap operation
     *
     * @return true if the tap connection was valid and the operation
     *         was performed
     */
    template <typename V>
    bool performTapOp(const std::string &name, TapOperation<V> &tapop, V arg) {
        bool shouldNotify(true);
        bool clear(true);
        bool ret(true);
        LockHolder lh(notifySync);

        TapConnection *tc = findByName_UNLOCKED(name);
        if (tc) {
            TapProducer *tp = dynamic_cast<TapProducer*>(tc);
            assert(tp != NULL);
            tapop.perform(tp, arg);
            shouldNotify = isPaused(tp);
            clear = shouldDisconnect(tc);
        } else {
            ret = false;
        }

        if (clear) {
            clearValidity(name);
        }

        if (shouldNotify) {
            notifySync.notify();
        }

        return ret;
    }

    /**
     * Clear the tap validity for the given named connection.
     */
    void clearValidity(const std::string &name);

    /**
     * Set named tap validity for the given token.
     */
    void setValidity(const std::string &name, const void* token);

    /**
     * Return true if the given name is valid for the given token.
     */
    bool checkValidity(const std::string &name, const void* token);

    /**
     * Return true if the TAP connection with the given name is still alive
     */
    bool checkConnectivity(const std::string &name);

    /**
     * Increments reference count of validity token (cookie in
     * fact). NOTE: takes notifySync lock.
     */
    ENGINE_ERROR_CODE reserveValidityToken(const void *token);

    /**
     * Decrements and posibly frees/invalidate validity token (cookie
     * in fact). NOTE: this acquires notifySync lock.
     */
    void releaseValidityToken(const void *token);

    /**
     * Return true if the backfill is completed for a given TAP connection.
     */
    bool checkBackfillCompletion(const std::string &name);

    /**
     * Set some backfilled events for a named conn.
     */
    bool setEvents(const std::string &name,
                   std::list<queued_item> *q);

    /**
     * Get the size of the named backfill queue.
     *
     * @return the size, or -1 if we can't find the queue
     */
    ssize_t backfillQueueDepth(const std::string &name);

    /**
     * Add an event to all tap connections telling them to flush their
     * items.
     */
    void addFlushEvent();

    /**
     * Notify anyone who's waiting for tap stuff.
     */
    void notify() {
        LockHolder lh(notifySync);
        notifySync.notify();
    }

    void wait(double howlong) {
        // Prevent the notify thread from busy-looping while
        // holding locks when there's work to do.
        LockHolder lh(notifySync);
        notifySync.wait(howlong);
    }

    /**
     * Find or build a tap connection for the given cookie and with
     * the given name.
     */
    TapProducer *newProducer(const void* cookie,
                             const std::string &name,
                             uint32_t flags,
                             uint64_t backfillAge,
                             int tapKeepAlive);

    /**
     * Create a new consumer and add it in the list of TapConnections
     * @param e the engine
     * @param c the cookie representing the client
     * @return Pointer to the nw tap connection
     */
    TapConsumer *newConsumer(const void* c);

    /**
     * Call a function on each tap connection.
     */
    template <typename Fun>
    void each(Fun f) {
        LockHolder lh(notifySync);
        each_UNLOCKED(f);
    }

    /**
     * Call a function on each tap connection *without* a lock.
     */
    template <typename Fun>
    void each_UNLOCKED(Fun f) {
        std::for_each(all.begin(), all.end(), f);
    }

    /**
     * Return the number of connections for which this predicate is true.
     */
    template <typename Fun>
    size_t count_if(Fun f) {
        LockHolder lh(notifySync);
        return count_if_UNLOCKED(f);
    }

    /**
     * Return the number of connections for which this predicate is
     * true *without* a lock.
     */
    template <typename Fun>
    size_t count_if_UNLOCKED(Fun f) {
        return static_cast<size_t>(std::count_if(all.begin(), all.end(), f));
    }

    /**
     * Notify the tap connections.
     *
     * @return true if we need need to rush another run in quickly
     */
    void notifyIOThreadMain();

    bool SetCursorToOpenCheckpoint(const std::string &name, uint16_t vbucket);

    bool closeTapConnectionByName(const std::string &name);

    void shutdownAllTapConnections();

    void scheduleBackfill(const std::set<uint16_t> &backfillVBuckets);

    void resetReplicaChain();

private:

    TapConnection *findByName_UNLOCKED(const std::string &name);
    void getExpiredConnections_UNLOCKED(std::list<TapConnection*> &deadClients,
                                        std::list<TapConnection*> &regClients);

    void removeTapCursors_UNLOCKED(TapProducer *tp);

    bool mapped(TapConnection *tc);

    bool isPaused(TapProducer *tc);
    bool shouldDisconnect(TapConnection *tc);

    SyncObject                               notifySync;
    std::map<const void*, TapConnection*>    map;
    std::map<const std::string, const void*> validity;
    std::list<TapConnection*>                all;

    /* Handle to the engine who owns us */
    EventuallyPersistentEngine &engine;
};

#endif /* TAPCONNMAP_HH */
