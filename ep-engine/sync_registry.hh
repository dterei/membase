/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011 Couchbase, Inc.
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
#ifndef SYNC_REGISTRY_HH
#define SYNC_REGISTRY_HH 1

#include <set>
#include <list>
#include <map>
#include <iostream>

#include "common.hh"
#include "dispatcher.hh"
#include "item.hh"
#include "mutex.hh"
#include "queueditem.hh"

typedef struct key_spec_t {
    key_spec_t(const queued_item &qi)
        : cas(0), vbucketid(qi->getVBucketId()), key(qi->getKey()) {
    }

    key_spec_t(const Item &it)
        : cas(it.getCas()), vbucketid(it.getVBucketId()), key(it.getKey()) {
    }

    key_spec_t(uint64_t aCas, uint16_t vbid, std::string aKey)
        : cas(aCas), vbucketid(vbid), key(aKey) {
    }

    uint64_t cas;
    uint16_t vbucketid;
    std::string key;
    bool operator<(const key_spec_t &other) const {
        return (key < other.key) || ((key == other.key) && (vbucketid < other.vbucketid));
    }
} key_spec_t;

std::ostream& operator << (std::ostream& os, const key_spec_t &keyspec);

typedef enum {
    PERSIST,
    MUTATION,
    REP,
    REP_OR_PERSIST,
    REP_AND_PERSIST
} sync_type_t;


class EventuallyPersistentEngine;
class SyncListener;


/**
 * Dispatcher task to kill a SYNC command connection if it's waiting for
 * too long.
 */
class SyncAbortCallback : public DispatcherCallback {
public:
    SyncAbortCallback(SyncListener &list) : listener(list) {
    }

    bool callback(Dispatcher &, TaskId);

    std::string description() {
        return "SyncListener abort callback";
    }

    hrtime_t maxExpectedDuration();

private:
    SyncListener &listener;
};

/**
 * Dispatcher task to safely shut down synchronization tasks.
 */
class SyncDestructionCallback : public DispatcherCallback {
public:
    SyncDestructionCallback(SyncListener *sl) : syncListener(sl) {
    }

    bool callback(Dispatcher &, TaskId);

    std::string description() {
        return "SyncListener destruction callback";
    }

private:
    SyncListener *syncListener;
};

/**
 * Registers listeners for the Sync commands (persistence sync and replication sync).
 */
class SyncRegistry {
public:

    SyncRegistry() {
    }

    void addPersistenceListener(SyncListener *syncListener);
    void removePersistenceListener(SyncListener *syncListener);
    void itemPersisted(const queued_item &item);
    void itemsPersisted(std::list<queued_item> &itemlist);

    void addMutationListener(SyncListener *syncListener);
    void removeMutationListener(SyncListener *syncListener);
    void itemModified(const key_spec_t &keyspec);
    void itemDeleted(const key_spec_t &keyspec);

    void addReplicationListener(SyncListener *syncListener);
    void removeReplicationListener(SyncListener *syncListener);
    void itemReplicated(const key_spec_t &keyspec, uint8_t replicaCount = 1);

private:

    void notifyListeners(std::set<SyncListener*> &listeners,
                         const key_spec_t &keyspec,
                         bool deleted);

    void notifyListeners(std::set<SyncListener*> &listeners,
                         const key_spec_t &keyspec,
                         uint8_t replicaCount);

    std::set<SyncListener*> persistenceListeners;
    Mutex persistenceMutex;

    std::set<SyncListener*> mutationListeners;
    Mutex mutationMutex;

    std::set<SyncListener*> replicationListeners;
    Mutex replicationMutex;

    DISALLOW_COPY_AND_ASSIGN(SyncRegistry);
};


class SyncListener {
public:

    SyncListener(EventuallyPersistentEngine &epEngine,
                 const void *c,
                 std::set<key_spec_t> *keys,
                 sync_type_t sync_type,
                 uint8_t replicaCount = 0);

    void keySynced(const key_spec_t &keyspec, bool deleted = false);
    void keySynced(const key_spec_t &keyspec, uint8_t numReplicas);

    void maybeNotifyIOComplete(bool timedout = false);
    bool maybeEnableNotifyIOComplete();

    sync_type_t getSyncType() const {
        return syncType;
    }

    bool isFinished() const {
        return finished;
    }

    std::set<key_spec_t>& getPersistedKeys() {
        return persistedKeys;
    }

    std::set<key_spec_t>& getModifiedKeys() {
        return modifiedKeys;
    }

    std::set<key_spec_t>& getDeletedKeys() {
        return deletedKeys;
    }

    std::set<key_spec_t>& getReplicatedKeys() {
        return replicatedKeys;
    }

    std::set<key_spec_t>& getNonExistentKeys() {
        return nonExistentKeys;
    }

    std::set<key_spec_t>& getInvalidCasKeys() {
        return invalidCasKeys;
    }

    /**
     * Request destruction of this SyncListener.
     *
     * This is more complicated than a normal destructor because we
     * need to ensure it runs on the right thread.
     */
    void destroy();

private:

    friend class SyncAbortCallback;
    friend class SyncDestructionCallback;

    ~SyncListener();

    EventuallyPersistentEngine   &engine;
    const void                   *cookie;
    std::set<key_spec_t>         *keySpecs;
    size_t                       persistedOrReplicated;
    TaskId                       abortTaskId;
    Mutex                        mutex;
    const hrtime_t               startTime;
    std::set<key_spec_t>         persistedKeys;
    std::set<key_spec_t>         modifiedKeys;
    std::set<key_spec_t>         deletedKeys;
    std::set<key_spec_t>         replicatedKeys;
    std::map<key_spec_t,uint8_t> replicaCounts;
    std::set<key_spec_t>         nonExistentKeys;
    std::set<key_spec_t>         invalidCasKeys;
    sync_type_t                  syncType;
    const uint8_t                replicasPerKey;
    bool                         finished;
    bool                         allowNotify;
};


#endif /* SYNC_REGISTRY_HH */
