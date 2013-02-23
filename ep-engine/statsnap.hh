/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef STATSNAP_HH
#define STATSNAP_HH 1

#include "common.hh"

#include <string>
#include <map>

#include "dispatcher.hh"
#include "stats.hh"

const int STATSNAP_FREQ(60);

// Forward declaration.
class EventuallyPersistentEngine;

/**
 * Periodically take a snapshot of the stats and record it in the main
 * DB.
 */
class StatSnap : public DispatcherCallback {
public:
    StatSnap(EventuallyPersistentEngine *e) : engine(e) { }

    bool callback(Dispatcher &d, TaskId t);

    /**
     * Grab stats from the engine.
     *
     * @param stats the optional stat parameter
     * @return true if the stat fetch was successful
     */
    bool getStats(const char *stats=NULL);

    /**
     * Get the current map of data.
     */
    std::map<std::string, std::string> &getMap() { return map; }

    /**
     * Description of task.
     */
    std::string description() {
        std::string rv("Updating stat snapshot on disk");
        return rv;
    }

private:
    EventuallyPersistentEngine         *engine;
    std::map<std::string, std::string>  map;
};

#endif /* STATSNAP_HH */
