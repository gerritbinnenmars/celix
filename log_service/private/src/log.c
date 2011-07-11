/**
 *Licensed to the Apache Software Foundation (ASF) under one
 *or more contributor license agreements.  See the NOTICE file
 *distributed with this work for additional information
 *regarding copyright ownership.  The ASF licenses this file
 *to you under the Apache License, Version 2.0 (the
 *"License"); you may not use this file except in compliance
 *with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *Unless required by applicable law or agreed to in writing,
 *software distributed under the License is distributed on an
 *"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 *specific language governing permissions and limitations
 *under the License.
 */
/*
 * log.c
 *
 *  Created on: Jun 26, 2011
 *      Author: alexander
 */
#include <apr_thread_cond.h>
#include <apr_thread_mutex.h>
#include <apr_thread_proc.h>

#include "log.h"
#include "linked_list_iterator.h"
#include "array_list.h"

struct log {
    LINKED_LIST entries;
    apr_thread_mutex_t *lock;

    ARRAY_LIST listeners;
    ARRAY_LIST listenerEntries;

    apr_thread_t *listenerThread;
    bool running;

    apr_thread_cond_t *entriesToDeliver;
    apr_thread_mutex_t *deliverLock;
    apr_thread_mutex_t *listenerLock;

    apr_pool_t *pool;
};

celix_status_t log_startListenerThread(log_t logger);
celix_status_t log_stopListenerThread(log_t logger);

void * APR_THREAD_FUNC log_listenerThread(apr_thread_t *thread, void *data);

celix_status_t log_create(apr_pool_t *pool, log_t *logger) {
    celix_status_t status = CELIX_SUCCESS;

    *logger = apr_palloc(pool, sizeof(**logger));
    if (*logger == NULL) {
        status = CELIX_ENOMEM;
    } else {
        apr_status_t apr_status;

        linkedList_create(pool, &(*logger)->entries);
        apr_thread_mutex_create(&(*logger)->lock, APR_THREAD_MUTEX_UNNESTED, pool);

        (*logger)->pool = pool;
        (*logger)->listeners = arrayList_create();
        (*logger)->listenerEntries = arrayList_create();
        (*logger)->listenerThread = NULL;
        (*logger)->running = false;
        apr_status = apr_thread_cond_create(&(*logger)->entriesToDeliver, pool);
        if (apr_status != APR_SUCCESS) {
            status = CELIX_INVALID_SYNTAX;
        } else {
            apr_status = apr_thread_mutex_create(&(*logger)->deliverLock, 0, pool);
            if (apr_status != APR_SUCCESS) {
                status = CELIX_INVALID_SYNTAX;
            } else {
                apr_status = apr_thread_mutex_create(&(*logger)->listenerLock, 0, pool);
                if (apr_status != APR_SUCCESS) {
                    status = CELIX_INVALID_SYNTAX;
                } else {
                    // done
                }
            }
        }
    }

    return status;
}

celix_status_t log_addEntry(log_t log, log_entry_t entry) {
    apr_thread_mutex_lock(log->lock);
    linkedList_addElement(log->entries, entry);

    // notify any listeners
    if (log->listenerThread != NULL)
    {
        arrayList_add(log->listenerEntries, entry);
        apr_thread_cond_signal(log->entriesToDeliver);
    }

    apr_thread_mutex_unlock(log->lock);
    return CELIX_SUCCESS;
}

celix_status_t log_getEntries(log_t log, apr_pool_t *memory_pool, LINKED_LIST *list) {
    LINKED_LIST entries = NULL;
    if (linkedList_create(memory_pool, &entries) == CELIX_SUCCESS) {
        LINKED_LIST_ITERATOR iter = NULL;

        apr_thread_mutex_lock(log->lock);

        iter = linkedListIterator_create(log->entries, 0);
        while (linkedListIterator_hasNext(iter)) {
            linkedList_addElement(entries, linkedListIterator_next(iter));
        }

        *list = entries;

        apr_thread_mutex_unlock(log->lock);

        return CELIX_SUCCESS;
    } else {
        return CELIX_ENOMEM;
    }
}

celix_status_t log_addLogListener(log_t logger, log_listener_t listener) {
    celix_status_t status = CELIX_SUCCESS;
    apr_status_t apr_status;

    apr_status = apr_thread_mutex_lock(logger->listenerLock);
    if (apr_status != APR_SUCCESS) {
        status = CELIX_INVALID_SYNTAX;
    } else {
        arrayList_add(logger->listeners, listener);

        if (logger->listenerThread == NULL) {
            log_startListenerThread(logger);
        }

        apr_status = apr_thread_mutex_unlock(logger->listenerLock);
        if (apr_status != APR_SUCCESS) {
            status = CELIX_INVALID_SYNTAX;
        }
    }

    return status;
}

celix_status_t log_removeLogListener(log_t logger, log_listener_t listener) {
    celix_status_t status = CELIX_SUCCESS;
    apr_status_t apr_status;

    apr_status = apr_thread_mutex_lock(logger->listenerLock);
    if (apr_status != APR_SUCCESS) {
        status = CELIX_INVALID_SYNTAX;
    } else {
        arrayList_removeElement(logger->listeners, listener);
        if (arrayList_size(logger->listeners) == 0) {
            log_stopListenerThread(logger);
        }

        apr_status = apr_thread_mutex_unlock(logger->listenerLock);
        if (apr_status != APR_SUCCESS) {
            status = CELIX_INVALID_SYNTAX;
        }
    }

    return status;
}

celix_status_t log_removeAllLogListener(log_t logger) {
    celix_status_t status = CELIX_SUCCESS;

    apr_status_t apr_status;

    apr_status = apr_thread_mutex_lock(logger->listenerLock);
    if (apr_status != APR_SUCCESS) {
        status = CELIX_INVALID_SYNTAX;
    } else {
        arrayList_clear(logger->listeners);


        apr_status = apr_thread_mutex_unlock(logger->listenerLock);
        if (apr_status != APR_SUCCESS) {
            status = CELIX_INVALID_SYNTAX;
        }
    }

    return status;
}

celix_status_t log_startListenerThread(log_t logger) {
    celix_status_t status = CELIX_SUCCESS;
    apr_status_t apr_status;

    logger->running = true;
    apr_status = apr_thread_create(&logger->listenerThread, NULL, log_listenerThread, logger, logger->pool);
    if (apr_status != APR_SUCCESS) {
        status = CELIX_INVALID_SYNTAX;
    }

    return status;
}

celix_status_t log_stopListenerThread(log_t logger) {
    celix_status_t status = CELIX_SUCCESS;
    apr_status_t apr_status = APR_SUCCESS;

    apr_status = apr_thread_mutex_lock(logger->deliverLock);
    if (apr_status != APR_SUCCESS) {
        status = CELIX_INVALID_SYNTAX;
    } else {
        apr_status_t stat;
        logger->running = false;
        apr_thread_cond_signal(logger->entriesToDeliver);
        status = apr_thread_mutex_unlock(logger->deliverLock);
        if (status != APR_SUCCESS) {
            status = CELIX_INVALID_SYNTAX;
        } else {
            //apr_thread_join(&stat, logger->listenerThread);
            logger->listenerThread = NULL;
        }

    }

    return status;
}

void * APR_THREAD_FUNC log_listenerThread(apr_thread_t *thread, void *data) {
    apr_status_t status = APR_SUCCESS;

    log_t logger = data;

    while (logger->running) {
        status = apr_thread_mutex_lock(logger->deliverLock);
        if (status != APR_SUCCESS) {
            logger->running = false;
        } else {
            if (!arrayList_isEmpty(logger->listenerEntries)) {
                log_entry_t entry = (log_entry_t) arrayList_remove(logger->listenerEntries, 0);

                status = apr_thread_mutex_lock(logger->listenerLock);
                if (status != APR_SUCCESS) {
                    logger->running = false;
                    break;
                } else {
                    ARRAY_LIST_ITERATOR it = arrayListIterator_create(logger->listeners);
                    while (arrayListIterator_hasNext(it)) {
                        log_listener_t listener = arrayListIterator_next(it);
                        listener->logged(listener, entry);
                    }

                    status = apr_thread_mutex_unlock(logger->listenerLock);
                    if (status != APR_SUCCESS) {
                        logger->running = false;
                        break;
                    }
                }
            }

            if (arrayList_isEmpty(logger->listenerEntries)) {
                apr_thread_cond_wait(logger->entriesToDeliver, logger->deliverLock);
            }

            status = apr_thread_mutex_unlock(logger->deliverLock);
            if (status != APR_SUCCESS) {
                logger->running = false;
                break;
            }
        }
    }

    apr_thread_exit(thread, status);
    return NULL;
}
