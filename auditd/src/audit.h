/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
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
#pragma once

#include "auditconfig.h"
#include "auditd.h"
#include "auditfile.h"

#include <memcached/audit_interface.h>

#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>

class Event;
class EventDescriptor;
struct cJSON;

class Audit {
public:
    AuditConfig config;
    std::map<uint32_t,EventDescriptor*> events;

    // We maintain two Event queues. At any one time one will be used to accept
    // new events, and the other will be processed. The two queues are swapped
    // periodically.
    std::unique_ptr<std::queue<Event*>> processeventqueue =
            std::make_unique<std::queue<Event*>>();
    std::unique_ptr<std::queue<Event*>> filleventqueue =
            std::make_unique<std::queue<Event*>>();

    bool terminate_audit_daemon = {false};
    std::string configfile;
    cb_thread_t consumer_tid = {};
    std::atomic_bool consumer_thread_running = {false};
    std::condition_variable events_arrived;
    std::mutex producer_consumer_lock;
    static std::string hostname;
    AuditFile auditfile;
    std::atomic<uint32_t> dropped_events = {0};

    explicit Audit(std::string config_file,
                   SERVER_COOKIE_API* sapi,
                   const std::string& host);
    ~Audit();

    bool initialize_event_data_structures(cJSON *event_ptr);
    bool process_module_data_structures(cJSON *module);
    bool process_module_descriptor(cJSON *module_descriptor);
    bool configure();
    bool add_to_filleventqueue(uint32_t event_id,
                               cb::const_char_buffer payload);

    bool add_reconfigure_event(const char *configfile, const void *cookie);
    bool create_audit_event(uint32_t event_id, nlohmann::json& payload);
    bool terminate_consumer_thread();
    void clear_events_map();
    void clear_events_queues();
    bool clean_up();

    static void log_error(const AuditErrorCode return_code,
                          const std::string& string = "");

    /**
     * Add a listener to notify state changes for individual events.
     *
     * @param listener the callback function
     */
    void add_event_state_listener(cb::audit::EventStateListener listener);

    void notify_all_event_states();

    void notify_io_complete(gsl::not_null<const void*> cookie,
                            ENGINE_ERROR_CODE status);

    /**
     * Add all statistics from the audit daemon
     *
     * @param add_stats The callback function to add the variable to the
     *                  stream to the clients
     * @param cookie The cookie used to identify the client
     */
    void stats(ADD_STAT add_stats, gsl::not_null<const void*> cookie);

protected:
    void notify_event_state_changed(uint32_t id, bool enabled) const;
    struct {
        mutable std::mutex mutex;
        std::vector<cb::audit::EventStateListener> clients;
    } event_state_listener;

    SERVER_COOKIE_API* cookie_api;

private:
    const size_t max_audit_queue = 50000;
};

