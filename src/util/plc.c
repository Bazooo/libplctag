/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/libplctag.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/event_loop.h>
#include <util/mem.h>
#include <util/mutex.h>
#include <util/plc.h>
#include <util/sleep.h>
#include <util/string.h>
#include <util/socket.h>
#include <util/time.h>
#include <util/timer_event.h>


#define DESTROY_DISCONNECT_TIMEOUT_MS (500)
#define DEFAULT_IDLE_TIMEOUT_MS (5000)
#define PLC_HEARTBEAT_INTERVAL_MS (200)
#define MAX_RETRY_INTERVAL_MS (16000)
#define MIN_RETRY_INTERVAL_MS (1000)


/* very common code patterns. */

#define NEXT_STATE(state) do { \
    pdebug(DEBUG_INFO, "Next state %s for PLC %s.", #state, plc->key); \
    plc->state_func = state; \
} while(0);

#define DISCONNECT_ON_ERROR(CONDITION, ...)  \
if(CONDITION) { \
    pdebug(DEBUG_WARN, __VA_ARGS__ ); \
    plc->retry_interval_ms *= 2; \
    if(plc->retry_interval_ms > MAX_RETRY_INTERVAL_MS) { \
        plc->retry_interval_ms = MAX_RETRY_INTERVAL_MS; \
    } \
    pdebug(DEBUG_WARN, "Retry in %dms.", plc->retry_interval_ms); \
    plc->next_retry_time = time_ms() + plc->retry_interval_ms; \
    NEXT_STATE(state_start_disconnect); \
    rc = PLCTAG_STATUS_PENDING; \
    break; \
}

#define RESET_ON_ERROR(CONDITION, ...) \
if(CONDITION) { \
    pdebug(DEBUG_WARN, __VA_ARGS__ ); \
    reset_plc(plc); \
    plc->retry_interval_ms *= 2; \
    if(plc->retry_interval_ms > MAX_RETRY_INTERVAL_MS) { \
        plc->retry_interval_ms = MAX_RETRY_INTERVAL_MS; \
    } \
    pdebug(DEBUG_WARN, "Retry in %dms.", plc->retry_interval_ms); \
    plc->next_retry_time = time_ms() + plc->retry_interval_ms; \
    NEXT_STATE(state_dispatch_requests); \
    rc = PLCTAG_STATUS_OK; \
    break; \
}

#define CONTINUE_UNLESS(CONDITION, ...)  \
if(CONDITION) { \
    pdebug(DEBUG_INFO, __VA_ARGS__ ); \
    NEXT_STATE(state_dispatch_requests); \
    rc = PLCTAG_STATUS_PENDING; \
    break; \
}

#define SPURIOUS_WAKEUP_IF(CONDITION, ...)  \
if(CONDITION) { \
    pdebug(DEBUG_INFO, __VA_ARGS__ ); \
    rc = PLCTAG_STATUS_OK; \
    break; \
}

#define CHECK_TERMINATION() \
if(plc->is_terminating == true) { \
    pdebug(DEBUG_DETAIL, "PLC %s is terminating.", plc->key); \
    NEXT_STATE(state_dispatch_requests); \
    rc = PLCTAG_STATUS_PENDING; \
    break; \
}





struct plc_s {
    struct plc_s *next;

    /* the unique identifying key for this PLC instance. */
    const char *key;

    mutex_p plc_mutex;

    bool is_terminating;

    /* where are we connected/connecting? */
    const char *host;
    int port;
    sock_p socket;

    /* our requests. */
    struct plc_request_s *request_list;
    plc_request_id current_request_id;

    /* the protocol layers make up the stack underneath the PLC. */
    struct plc_layer_s *top_layer;

    /* state engine */
    int (*state_func)(plc_p plc);

    /* data buffer. */
    uint8_t *data;
    int data_capacity;
    int payload_end;
    int payload_start;

    /* model-specific data. */
    void *context;
    void (*context_destructor)(plc_p plc, void *context);

    /* heartbeat timer */
    timer_p heartbeat_timer;

    /* error handling */
    int retry_interval_ms;
    int64_t next_retry_time;

    /* idle handling. */
    int idle_timeout_ms;
    int64_t next_idle_timeout;

    /* make this easily checked. */
    bool is_connected;
};


static mutex_p plc_list_mutex = NULL;
static plc_p plc_list = NULL;


static void reset_plc(plc_p plc);
static void plc_rc_destroy(void *plc_arg);
static void plc_state_runner(plc_p plc);
static void plc_heartbeat(plc_p plc);


/******** STATES *********/

/* main dispatch states. */
static int state_dispatch_requests(plc_p plc);
static int state_reserve_space_for_tag_request(plc_p plc);
static int state_build_tag_request(plc_p plc);
static int state_tag_request_sent(plc_p plc);
static int state_tag_response_ready(plc_p plc);

/* connection states. */
static int state_start_connect(plc_p plc);
static int state_build_layer_connect_request(plc_p plc);
static int state_layer_connect_request_sent(plc_p plc);
static int state_layer_connect_response_ready(plc_p plc);

/* disconnect states. */
static int state_start_disconnect(plc_p plc);
static int state_build_layer_disconnect_request(plc_p plc);
static int state_layer_disconnect_request_sent(plc_p plc);
static int state_layer_disconnect_response_ready(plc_p plc);

/* terminal or error states */
static int state_terminate(plc_p plc);



/*
 * plc_get
 *
 * Primary entry point to get a PLC.  This is called by PLC-specific
 * creation functions which will create the generic PLC and then
 * set up the layers.
 */

int plc_get(const char *plc_type, attr attribs, plc_p *plc_return, int (*constructor)(plc_p plc, attr attribs))
{
    int rc = PLCTAG_STATUS_OK;
    const char *gateway = NULL;
    const char *path = NULL;
    const char *plc_key = NULL;

    pdebug(DEBUG_INFO, "Starting for PLC type %s.", plc_type);

    *plc_return = NULL;

    /* build the search key. */
    gateway = attr_get_str(attribs, "gateway", NULL);
    if(!gateway || str_length(gateway) == 0) {
        pdebug(DEBUG_WARN, "Gateway host missing or zero length!");
        return PLCTAG_ERR_BAD_GATEWAY;
    }

    path = attr_get_str(attribs, "path", "NO_PATH");

    plc_key = str_concat(plc_type, "/", gateway, "/", path, NULL);
    if(!plc_key) {
        pdebug(DEBUG_WARN, "Unable to allocate plc_key!");
        return PLCTAG_ERR_NO_MEM;
    }

    critical_block(plc_list_mutex) {
        plc_p plc = NULL;

        /* try to find the PLC */
        for(plc=plc_list; plc; plc = plc->next) {
            if(str_cmp_i(plc_key, plc->key) != 0) {
                /* found it. */
                break;
            }
        }

        /* try to get a reference if we found one. */
        plc = rc_inc(plc);

        /* now check if we got a valid reference. */
        if(!plc) {
            const char *host_args = NULL;
            char **host_segments = NULL;
            int port = 0;

            /* need to make one. */
            plc = rc_alloc(sizeof(*plc), plc_rc_destroy);
            if(!plc) {
                pdebug(DEBUG_WARN, "Unable to allocate memory for new PLC %s!", plc_key);
                mem_free(plc_key);
                plc = NULL;
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            plc->key = plc_key;
            plc_key = NULL; /* prevent it from being deleted below */

            /* do as much as possible in advance. */

            /* we need a mutex in each PLC */
            rc = mutex_create(&(plc->plc_mutex));
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to create PLC mutex for PLC %s!", plc->key);
                plc = rc_dec(plc);
                break;
            }

            /* build the layers */
            rc = constructor(plc, attribs);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to construct PLC %s layers, error %s!", plc_key, plc_tag_decode_error(rc));
                plc = rc_dec(plc);
                break;
            }

            /* handle the host and port. Do this after the plc-specific constructor ran as it will set a default port. */
            host_args = attr_get_str(attribs, "gateway", NULL);
            port = attr_get_int(attribs, "default_port", 0);

            if(!host_args || str_length(host_args) == 0) {
                pdebug(DEBUG_WARN, "Host/gateway not provided!");
                plc = rc_dec(plc);
                rc = PLCTAG_ERR_BAD_GATEWAY;
                break;
            }

            host_segments = str_split(host_args, ":");

            if(!host_segments) {
                pdebug(DEBUG_WARN, "Unable to split gateway string!");
                plc = rc_dec(plc);
                rc = PLCTAG_ERR_BAD_GATEWAY;
                break;
            }

            if(!host_segments[0] || str_length(host_segments[0]) == 0) {
                pdebug(DEBUG_WARN, "Host/gateway not provided!");
                mem_free(host_segments);
                plc = rc_dec(plc);
                rc = PLCTAG_ERR_BAD_GATEWAY;
                break;
            }

            plc->host = str_dup(host_segments[0]);

            if(host_segments[1] && str_length(host_segments[1])) {
                rc = str_to_int(host_segments[1], &port);
                if(rc != PLCTAG_STATUS_OK) {
                    pdebug(DEBUG_WARN, "Unable to parse port number, error %s!", plc_tag_decode_error(rc));
                    mem_free(host_segments);
                    plc = rc_dec(plc);
                    rc = PLCTAG_ERR_BAD_GATEWAY;
                    break;
                }

                if(port <= 0 || port > 65535) {
                    pdebug(DEBUG_WARN, "Port value (%d) must be between 0 and 65535!", port);
                    mem_free(host_segments);
                    plc = rc_dec(plc);
                    rc = PLCTAG_ERR_BAD_GATEWAY;
                    break;
                }
            }

            mem_free(host_segments);

            /* the PLC-specific constructor can override this if needed or the application can. */
            plc->idle_timeout_ms = attr_get_int(attribs, "idle_timeout_ms", DEFAULT_IDLE_TIMEOUT_MS);

            /* as a last step, set up the heartbeat timer. */
            rc = timer_event_create(&(plc->heartbeat_timer));
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to create heartbeat_timer, error %s!", plc_tag_decode_error(rc));
                plc = rc_dec(plc);
                break;
            }

            /* start the idle heartbeat_timer */
            rc = timer_event_wake_at(plc->heartbeat_timer, time_ms() + PLC_HEARTBEAT_INTERVAL_MS, (void (*)(void*))plc_heartbeat, plc);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Unable to start heartbeat timer, error %s for PLC %s!", plc_tag_decode_error(rc), plc->key);

                /* disable the timer event in case it is running */
                timer_event_snooze(plc->heartbeat_timer);

                plc = rc_dec(plc);
                break;
            }

            plc->port = port;

            /* start the state machine. */
            plc->state_func = state_dispatch_requests;

            /* put the PLC on the list. */
            plc->next = plc_list;
            plc_list = plc;
        }

        *plc_return = plc;
    }

    mem_free(plc_key);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to get or create PLC!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done for PLC type %s.", plc_type);

    return rc;
}



/*
 * plc_initialize
 *
 * Force a hard reset of the PLC object state.
 */

int plc_initialize(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        reset_plc(plc);
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}


int plc_add_layer(plc_p plc, plc_layer_p layer)
{
    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    /* add to the front of the list. */
    layer->next = plc->top_layer;
    plc->top_layer = layer;

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
}



void *plc_get_context(plc_p plc)
{
    void *res = NULL;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return NULL;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        res = plc->context;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
}



int plc_set_context(plc_p plc, void *context, void (*context_destructor)(plc_p plc, void *context))
{
    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        plc->context = context;
        plc->context_destructor = context_destructor;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
}



int plc_get_idle_timeout(plc_p plc)
{
    int res = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        res = plc->idle_timeout_ms;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
}



int plc_set_idle_timeout(plc_p plc, int timeout_ms)
{
    int res = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    if(timeout_ms < 0 || timeout_ms > 5000) { /* FIXME - MAGIC constant */
        pdebug(DEBUG_WARN, "Illegal timeout value %d!", timeout_ms);
        return PLCTAG_ERR_OUT_OF_BOUNDS;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        res = plc->idle_timeout_ms;
        plc->idle_timeout_ms = timeout_ms;
    }

    /* new timeout period takes effect after the next heartbeat_timer wake up. */

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
}



int plc_get_buffer_size(plc_p plc)
{
    int res = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        res = plc->payload_end;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return res;
}



int plc_set_buffer_size(plc_p plc, int buffer_size)
{
    int rc = 0;

    if(!plc) {
        pdebug(DEBUG_WARN, "PLC pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    if(buffer_size <= 0) {
        pdebug(DEBUG_WARN, "Illegal buffer size value %d, must be a positive number of bytes!", buffer_size);
        return PLCTAG_ERR_TOO_SMALL;
    }

    /* only resize up for now. */
    /* TODO - do we want to allow downsizing the buffer? */
    critical_block(plc->plc_mutex) {
        /* do we need to resize the buffer? */
        if(buffer_size > plc->data_capacity) {
            pdebug(DEBUG_DETAIL, "Resizing the buffer from %d bytes to %d bytes.", plc->data_capacity, buffer_size);

            plc->data = mem_realloc(plc->data, buffer_size);
            if(!plc->data) {
                pdebug(DEBUG_WARN, "Unable to reallocate memory for data buffer!");
                rc = PLCTAG_ERR_NO_MEM;
                break;
            }

            plc->data_capacity = buffer_size;
        } else {
            pdebug(DEBUG_DETAIL, "Buffer already %d which is greater than the requested %d bytes.", plc->data_capacity, buffer_size);
        }
    }

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error setting buffer size!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
}




int plc_start_request(plc_p plc,
                      plc_request_p request,
                      void *client,
                      int (*build_request_callback)(void *client, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_num),
                      int (*process_response_callback)(void *client, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_num))
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        /* is the request already on the list?  If not, add it at the end. */
        plc_request_p *walker = &(plc->request_list);

        while(*walker && *walker != request) {
            walker = &((*walker)->next);
        }

        if(*walker) {
            /* we found the request, already busy. */
            pdebug(DEBUG_WARN, "Request is already queued!");
            rc = PLCTAG_ERR_BUSY;
            break;
        }

        /* did not find, it, so add it and continue processing. */
        request->next = NULL;
        request->req_id = -1;
        request->context = client;
        request->build_request = build_request_callback;
        request->process_response = process_response_callback;

        *walker = request;

        /* call the runner if we are dispatching */
        if(plc->state_func == state_dispatch_requests) {
            plc_state_runner(plc);
        }
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);
    } else {
        pdebug(DEBUG_INFO, "Done with error %s for PLC %s.", plc_tag_decode_error(rc), plc->key);
    }

    return PLCTAG_STATUS_OK;
}





int plc_stop_request(plc_p plc, plc_request_p request)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        /* is the request already on the list?  If not, add it at the end. */
        plc_request_p *walker = &(plc->request_list);

        while(*walker && *walker != request) {
            walker = &((*walker)->next);
        }

        if(*walker) {
            *walker = request->next;
        } else {
            pdebug(DEBUG_INFO, "Request not on the PLC's list.");
            rc = PLCTAG_ERR_NOT_FOUND;
        }

        /* TODO - do we need to call the main statemachine loop here? */
    }

    if(rc == PLCTAG_STATUS_OK) {
        pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);
    } else {
        pdebug(DEBUG_INFO, "Done with error %s for PLC %s.", plc_tag_decode_error(rc), plc->key);
    }

    return rc;
}

const char *plc_get_key(plc_p plc)
{
    if(plc) {
        return plc->key;
    } else {
        return "NULL PLC KEY!";
    }
}


void plc_rc_destroy(void *plc_arg)
{
    int rc = PLCTAG_STATUS_OK;
    plc_p plc = (plc_p)plc_arg;
    int64_t timeout = 0;
    bool is_connected = true;

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_INFO, "Remove PLC %s from the list.", plc->key);

    /* remove it from the list. */
    critical_block(plc_list_mutex) {
        plc_p *walker = &plc_list;

        while(*walker && *walker != plc) {
            walker = &((*walker)->next);
        }

        if(*walker == plc) {
            *walker = plc->next;
            plc->next = NULL;
        } else {
            pdebug(DEBUG_WARN, "PLC %s not found on the list!", plc->key);
        }
    }

    pdebug(DEBUG_INFO, "Stop PLC %s heartbeat.", plc->key);

    /* stop the heartbeat. */
    if(plc->heartbeat_timer) {
        timer_event_snooze(plc->heartbeat_timer);
        timer_destroy(&(plc->heartbeat_timer));
        plc->heartbeat_timer = NULL;
    }

    pdebug(DEBUG_INFO, "Start PLC %s disconnect.", plc->key);

    /* lock the PLC mutex for some of these operations. */
    critical_block(plc->plc_mutex) {
        plc->is_terminating = true;

        plc->is_terminating = true;

        /*
         * if the PLC is connected, run the state loop to try to get through
         * any terminatable states.
         */
        if(plc->is_connected) {
            is_connected = true;
            plc_state_runner(plc);
        }
    }

    /* loop until we timeout or we are disconnected. */
    if(is_connected) {
        pdebug(DEBUG_INFO, "Waiting for PLC %s to disconnect.", plc->key);

        timeout = time_ms() + DESTROY_DISCONNECT_TIMEOUT_MS;

        while(is_connected && timeout > time_ms()) {
            critical_block(plc->plc_mutex) {
                is_connected = plc->is_connected;
            }

            if(is_connected) {
                sleep_ms(10);
            }
        }
    }

    pdebug(DEBUG_INFO, "Resetting PLC %s.", plc->key);

    /* reset the PLC to clean up more things. */
    reset_plc(plc);

    pdebug(DEBUG_INFO, "Destroying PLC %s socket.", plc->key);

    if(plc->socket) {
        socket_destroy(&(plc->socket));
        plc->socket = NULL;
    }

    pdebug(DEBUG_INFO, "Destroying PLC %s layers.", plc->key);

    /* if there is local context, destroy it. */
    if(plc->context) {
        if(plc->context_destructor) {
            plc->context_destructor(plc, plc->context);
        } else {
            mem_free(plc->context);
        }

        plc->context = NULL;
    }

    pdebug(DEBUG_INFO, "Destroying PLC %s mutex.", plc->key);

    rc = mutex_destroy(&(plc->plc_mutex));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, destroying PLC mutex!", plc_tag_decode_error(rc));
    }
    plc->plc_mutex = NULL;

    pdebug(DEBUG_INFO, "Cleaning up PLC %s request list.", plc->key);

    if(plc->request_list) {
        pdebug(DEBUG_WARN, "Request list is not empty!");
    }
    plc->request_list = NULL;

    pdebug(DEBUG_INFO, "Freeing PLC %s data buffer.", plc->key);

    if(plc->data) {
        mem_free(plc->data);
        plc->data = NULL;
    }

    pdebug(DEBUG_INFO, "Freeing PLC %s key memory.", plc->key);

    if(plc->key) {
        mem_free(plc->key);
        plc->key = NULL;
    }

    pdebug(DEBUG_INFO, "Done.");
}



void reset_plc(plc_p plc)
{
    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    if(plc->socket) {
        /*
         * this is safe because socket_close() acquires the socket mutex.
         * And that prevents the event loop thread from calling a callback at the
         * same time.   Once this is done, the event loop will not trigger any
         * callbacks against this PLC.
         */
        socket_close(plc->socket);
    }

    /* reset the layers */
    plc->top_layer->initialize(plc->top_layer);

    /* we are no longer connected. */
    plc->is_connected = false;

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);
}



/* new implementation */

void plc_state_runner(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    critical_block(plc->plc_mutex) {
        /* loop until we end up waiting for something. */
        do {
            rc = plc->state_func(plc);
        } while(rc == PLCTAG_STATUS_PENDING);
    }

    pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);
}



void plc_heartbeat(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t now = event_loop_time();

    pdebug(DEBUG_DETAIL, "Starting.");

    /* we do not want the PLC changing from another thread */
    critical_block(plc->plc_mutex) {
        /* Only dispatch if we can. */
        if(plc->state_func == state_dispatch_requests) {
            plc_state_runner(plc);
        }
    }

    rc = timer_event_wake_at(plc->heartbeat_timer, now + PLC_HEARTBEAT_INTERVAL_MS, (void (*)(void*))plc_heartbeat, plc);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to set up heartbeat_timer wake event.  Got error %s!", plc_tag_decode_error(rc));

        /* TODO - what do we do now?   Everything is broken... */
    }

    pdebug(DEBUG_DETAIL, "Done.");
}




/* dispatch states. */


int state_dispatch_requests(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int64_t now = event_loop_time();

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        /* if we are terminating, then close down. */
        if(plc->is_terminating) {
            if(plc->is_connected == true) {
                pdebug(DEBUG_INFO, "PLC terminating, starting disconnect.");
                NEXT_STATE(state_start_disconnect);
                rc = PLCTAG_STATUS_PENDING;
                break;
            } else {
                NEXT_STATE(state_terminate);
                rc = PLCTAG_STATUS_OK;
                break;
            }
        }

        /* check idle time. */
        if((plc->is_connected == true) && (plc->next_idle_timeout < now)) {
            pdebug(DEBUG_INFO, "Starting idle disconnect.");
            NEXT_STATE(state_start_disconnect);
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* check the error timeout. */
        if(plc->next_retry_time > event_loop_time()) {
            pdebug(DEBUG_DETAIL, "Punting because we have not waited long enough from previous error.");
            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* are there requests queued? */
        if(plc->request_list) {
            /* are we connected? */
            if(plc->is_connected == false) {
                NEXT_STATE(state_start_connect);
                rc = PLCTAG_STATUS_PENDING;
                break;
            }

            /* we have requests and we are connected, dispatch! */
            NEXT_STATE(state_reserve_space_for_tag_request);
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        /* nothing to do. Stay in the dispatcher and wait for something to happen. */
        NEXT_STATE(state_dispatch_requests);
        rc = PLCTAG_STATUS_OK;
    } while(0);

    if(rc == PLCTAG_STATUS_OK || rc == PLCTAG_STATUS_PENDING) {
        pdebug(DEBUG_DETAIL, "Done dispatching for PLC %s.", plc->key);
        return rc;
    } else {
        pdebug(DEBUG_WARN, "Unexpected error %s while trying to dispatch for PLC %s!", plc_tag_decode_error(rc), plc->key);
        return rc;
    }

    /* TODO - handle errors. */
}


int state_reserve_space_for_tag_request(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = 0;
    plc_request_id req_id = 0;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        /* reserve space in the layers for a request. Bottom up.*/
        rc = plc->top_layer->reserve_space(plc->top_layer, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s while preparing layers for tag request for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* payload boundaries. */
        plc->payload_start = data_start;
        plc->payload_end = data_end;
        plc->current_request_id = req_id;

        /* set up request */
        // plc->current_request = plc->request_list;
        // plc->current_request->req_id = req_id;

        NEXT_STATE(state_build_tag_request);
        rc = PLCTAG_STATUS_PENDING;
        break;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}


int state_build_tag_request(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = plc->payload_start;
    int data_end = plc->payload_end;
    int old_data_end = data_end;
    plc_request_id req_id = plc->current_request_id;
    struct plc_request_s *current_request = plc->request_list;
    bool done = true;
    bool first_try = true;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        CHECK_TERMINATION();

        /* repeat for all requests possible. */

        if(!current_request) {
            pdebug(DEBUG_INFO, "Request removed from the queue before we got to it!");

            plc->state_func = state_dispatch_requests;
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        pdebug(DEBUG_INFO, "Processing request %" REQ_ID_FMT ".", req_id);

        /* build the tag request on top of the reserved space. */
        rc = current_request->build_request(current_request->context, plc->data, plc->data_capacity, &data_start, &data_end, req_id);

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_TOO_SMALL, "Error %s building tag request for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* was there enough space? */
        if(rc == PLCTAG_ERR_TOO_SMALL) {
            DISCONNECT_ON_ERROR(first_try == true, "Insufficient space for even one new request!");

            pdebug(DEBUG_INFO, "Insufficient space to build for new request %" REQ_ID_FMT " for PLC %s.", req_id, plc->key);

            /* signal that we are unable to continue. */
            data_end = old_data_end;
        } else {
            pdebug(DEBUG_INFO, "Filling in layers for new request %" REQ_ID_FMT " for PLC %s.", req_id, plc->key);
            old_data_end = data_end;
        }

        first_try = false;

        /* set the request ID to match later. */
        current_request->req_id = req_id;

        /* build the layers. */
        rc = plc->top_layer->build_layer(plc->top_layer, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING, "Error %s while building request layers for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* if there was more space, see if we can find more requests. */
        if(rc == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_INFO, "Done setting up layers and no further packets allowed for PLC %s", plc->key);
            done = true;
        } else { /* rc == PLCTAG_STATUS_PENDING */
            /* check if we can find another request */
            current_request = current_request->next;

            /* check to see if we have a valid request. */
            if(current_request) {
                pdebug(DEBUG_INFO, "Another request is possible to pack for PLC %s.", plc->key);
                done = false;
            } else {
                pdebug(DEBUG_INFO, "Ran out of requests to handle.");
                data_end = old_data_end;
                done = true;
            }
        }

        if(done == true) {
            /* check before we commit to sending a packet. */
            CHECK_TERMINATION();

            pdebug(DEBUG_INFO, "Sending packet for PLC %s.", plc->key);

            plc->payload_end = data_end;

            NEXT_STATE(state_tag_request_sent);

            rc = socket_callback_when_write_done(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->data, &(plc->payload_end));

            DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s while setting up write completion callback for PLC %s!", plc_tag_decode_error(rc), plc->key);

            rc = PLCTAG_STATUS_OK;
            break;
        }
    } while(!done);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int state_tag_request_sent(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = 0;
    plc_request_id req_id = 0;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    do {
        /* find out why we got called. */
        rc = socket_status(plc->socket);

        SPURIOUS_WAKEUP_IF(rc == PLCTAG_STATUS_PENDING, "Spurious wakeup, socket write is still PENDING for PLC %s.", plc->key);

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s when trying to write socket in PLC %s!", plc_tag_decode_error(rc), plc->key)

        /* all good!  We send the request.  Prepare the layers for receiving the response. */
        rc = plc->top_layer->reserve_space(plc->top_layer, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s when trying to reserve space for response in PLC %s!", plc_tag_decode_error(rc), plc->key)

        /* ready to wait for the response. */
        NEXT_STATE(state_tag_response_ready);

        /* clear out the payload. */
        plc->payload_end = plc->payload_start = 0;

        rc = socket_callback_when_read_done(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->data, plc->data_capacity, &(plc->payload_end));

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s when trying to set up socket response read in PLC %s!", plc_tag_decode_error(rc), plc->key)

        /* wait for response. */
        rc = PLCTAG_STATUS_OK;
        break;
    } while(0);

    pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);

    return rc;
}



int state_tag_response_ready(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = plc->payload_end;
    plc_request_id req_id = 0;
    bool done = true;

    pdebug(DEBUG_DETAIL, "Starting for PLC %s.", plc->key);

    do {
        CHECK_TERMINATION();

        /* check socket status.  Might be an error */
        rc = socket_status(plc->socket);

        SPURIOUS_WAKEUP_IF(rc == PLCTAG_STATUS_PENDING, "Spurious wakeup, socket read is still PENDING for PLC %s.", plc->key);

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s from socket read for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /*
         * If a layer returns PLCTAG_STATUS_PENDING, then we know that there are multiple
         * sub-packets remaining within it and we need to continue.
         */

        rc = plc->top_layer->process_response(plc->top_layer, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK && rc != PLCTAG_ERR_PARTIAL, "Got error %s processing layers, restarting stack for PLC %s!", plc_tag_decode_error(rc), plc->key)

        /* if we get PLCTAG_ERR_PARTIAL, then we need to keep reading. */
        if(rc == PLCTAG_ERR_PARTIAL) {
            pdebug(DEBUG_INFO, "PLC %s did not get all the data yet, need to get more.", plc->key);

            /* come back to this routine. */
            NEXT_STATE(state_tag_response_ready);
            rc = socket_callback_when_read_done(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->data, plc->data_capacity, &(plc->payload_end));

            DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Unexpected error %s setting socket read callback for PLC %s!", plc_tag_decode_error(rc), plc->key);

            /* We know we need to wait, so there is nothing for the state runner to do. */
            rc = PLCTAG_STATUS_OK;
            break;
        }

        /* if we got here, then we have a valid response.
         * The data_start and data_end bracket the payload for the tag.
         *
         * req_id is the internal ID for the request we want to match.
         */

        /* are there any requests and is it the right one? */
        if(plc->request_list) {
            if(req_id == plc->request_list->req_id) {
                plc_request_p request = plc->request_list;

                /* remove the request from the queue. We have a response. */
                plc->request_list = plc->request_list->next;
                request->next = NULL;

                // data_start = plc->payload_start;
                // data_end = plc->payload_end;

                pdebug(DEBUG_DETAIL, "Attempting to process request %" REQ_ID_FMT " for PLC %s.", request->req_id, plc->key);

                // pdebug(DEBUG_DETAIL, "Got possible response:");
                // pdebug_dump_bytes(DEBUG_DETAIL, plc->data + data_start, data_end - data_start);

                rc = request->process_response(request->context, plc->data, plc->data_capacity, &data_start, &data_end, request->req_id);

                DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s processing request for tag for PLC %s!", plc_tag_decode_error(rc), plc->key);
            } else {
                /* nope */
                pdebug(DEBUG_INFO, "Skipping response for aborted request %" REQ_ID_FMT " for PLC %s.", plc->current_request_id, plc->key);
            }
        } else {
            pdebug(DEBUG_INFO, "No requests left to process for PLC %s.", plc->key);
            done = true;
        }

        if(done == true) {
            pdebug(DEBUG_INFO, "Finished processing response for PLC %s.", plc->key);
            NEXT_STATE(state_dispatch_requests);
            rc = PLCTAG_STATUS_PENDING;
            break;
        }
    } while(!done);

    pdebug(DEBUG_DETAIL, "Done for PLC %s.", plc->key);

    return rc;
}





/**** CONNECT STATES ****/

int state_start_connect(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        CHECK_TERMINATION();

        CONTINUE_UNLESS(plc->next_retry_time > event_loop_time(), "Retry time is not past for PLC %s.", plc->key);

        CONTINUE_UNLESS(plc->is_connected == true, "PLC %s is already connected!", plc->key);

        if(!plc->socket) {
            pdebug(DEBUG_INFO, "Creating socket.");

            rc = socket_create(&(plc->socket));
            RESET_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s while attempting to create socket object, resetting PLC %s!", plc_tag_decode_error(rc), plc->key);
        }

        /* initialize the layers */
        rc = plc->top_layer->initialize(plc->top_layer);
        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s initializing layers for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* kick off a connect. */
        NEXT_STATE(state_build_layer_connect_request);
        rc = socket_callback_when_connection_ready(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->host, plc->port);

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Got error %s, unable to start background socket connection for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* we are waiting on the socket to open, do not loop. */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int state_build_layer_connect_request(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        CHECK_TERMINATION();

        /* we can get here from the socket callback. Check the socket state. */
        rc = socket_status(plc->socket);

        SPURIOUS_WAKEUP_IF(rc == PLCTAG_STATUS_PENDING, "Spurious wakeup, socket connect is still PENDING for PLC %s.", plc->key);

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Connection failed with error %s for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* clean up state. */
        plc->payload_end = plc->payload_start = 0;

        /* build the connect request. */
        rc = plc->top_layer->connect(plc->top_layer, plc->data, plc->data_capacity, &(plc->payload_start), &(plc->payload_end));
        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING, "Error %s preparing connect attempt for layer for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* if we got a status of OK, then everything is connected. */
        if(rc == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_INFO, "All layers connected.");
            plc->is_connected = true;
            NEXT_STATE(state_dispatch_requests);
            break;
        }

        /* still work to do. */

        /* build layers below the currently connecting layer. */
        rc = plc->top_layer->build_layer(plc->top_layer, plc->data, plc->data_capacity, &(plc->payload_start), &(plc->payload_end), &(plc->current_request_id));
        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s fixing up layers for connect attempt for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* send the connect request. */
        NEXT_STATE(state_layer_connect_request_sent);

        rc = socket_callback_when_write_done(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->data, &(plc->payload_end));

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s setting up write callback for connect attempt for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* we wait. */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int state_layer_connect_request_sent(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        CHECK_TERMINATION();

        /* is the socket in good shape? */
        rc = socket_status(plc->socket);

        SPURIOUS_WAKEUP_IF(rc == PLCTAG_STATUS_PENDING, "Spurious wakeup, socket write is still PENDING for PLC %s.", plc->key);

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Connection request write failed with error %s for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* OK, so prep for receiving the response. */
        plc->payload_end = plc->payload_start = 0;

        NEXT_STATE(state_layer_connect_response_ready);
        rc = socket_callback_when_read_done(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->data, plc->data_capacity, &(plc->payload_end));

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s setting up read callback for connect response for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* wait */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int state_layer_connect_response_ready(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = plc->payload_end;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        CHECK_TERMINATION();

        /* is the socket in good shape? */
        rc = socket_status(plc->socket);

        SPURIOUS_WAKEUP_IF(rc == PLCTAG_STATUS_PENDING, "Spurious wakeup, socket read is still PENDING for PLC %s.", plc->key);

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Connection request read failed with error %s for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* check to see if we got the entire response back. */
        rc = plc->top_layer->process_response(plc->top_layer, plc->data, plc->data_capacity, &(data_start), &(data_end), &(plc->current_request_id));

        SPURIOUS_WAKEUP_IF(rc == PLCTAG_ERR_PARTIAL, "Partial packet received, continue to wait for PLC %s to send more data.", plc->key);

        if(rc == PLCTAG_STATUS_RETRY) {
            pdebug(DEBUG_INFO, "Connection attempt needs to be retried.");

            NEXT_STATE(state_build_layer_connect_request);
            rc = PLCTAG_STATUS_PENDING;
            break;
        }

        DISCONNECT_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s processing layer responses for connect response for PLC %s!", plc_tag_decode_error(rc), plc->key);

    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}




/********* DISCONNECT STATES *********/


int state_start_disconnect(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        CONTINUE_UNLESS(plc->is_connected == false, "PLC %s is already disconnected!", plc->key);

        /* initialize the layers */
        plc->payload_end = plc->payload_start = 0;

        NEXT_STATE(state_build_layer_disconnect_request)
        /* no wait */
        rc = PLCTAG_STATUS_PENDING;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int state_build_layer_disconnect_request(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = plc->data_capacity;
    plc_request_id req_id = 0;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        rc = plc->top_layer->disconnect(plc->top_layer, plc->data, plc->data_capacity, &data_start, &data_end);
        RESET_ON_ERROR(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING, "Error %s preparing layers for disconnect attempt for PLC %s!", plc_tag_decode_error(rc), plc->key);

        if(rc == PLCTAG_STATUS_OK) {
            pdebug(DEBUG_INFO, "Disconnect complete from PLC %s.", plc->key);
            plc->is_connected = false;
            break;
        }

        /* build the layers. */
        rc = plc->top_layer->build_layer(plc->top_layer, plc->data, plc->data_capacity, &data_start, &data_end, &req_id);
        RESET_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s fixing up layers for disconnect attempt for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* store the payload size. */
        plc->payload_start = 0;
        plc->payload_end = data_end;

        /* send the connect request. */
        NEXT_STATE(state_layer_disconnect_request_sent);

        rc = socket_callback_when_write_done(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->data, &(plc->payload_end));

        RESET_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s setting up write callback for disconnect attempt for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* we wait. */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int state_layer_disconnect_request_sent(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        /* is the socket in good shape? */
        rc = socket_status(plc->socket);

        SPURIOUS_WAKEUP_IF(rc == PLCTAG_STATUS_PENDING, "Spurious wakeup, socket write is still PENDING for PLC %s.", plc->key);

        RESET_ON_ERROR(rc != PLCTAG_STATUS_OK, "Disconnection request write failed with error %s for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* get ready to receive a response. */
        plc->payload_end = plc->payload_start = 0;

        NEXT_STATE(state_layer_disconnect_response_ready);
        rc = socket_callback_when_read_done(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->data, plc->data_capacity, &(plc->payload_end));

        RESET_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s setting up read callback for discconnect response for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* wait */
        rc = PLCTAG_STATUS_OK;
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}



int state_layer_disconnect_response_ready(plc_p plc)
{
    int rc = PLCTAG_STATUS_OK;
    int data_start = 0;
    int data_end = plc->payload_end;

    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    do {
        /* is the socket in good shape? */
        rc = socket_status(plc->socket);

        SPURIOUS_WAKEUP_IF(rc == PLCTAG_STATUS_PENDING, "Spurious wakeup, socket read is still PENDING for PLC %s.", plc->key);

        RESET_ON_ERROR(rc != PLCTAG_STATUS_OK, "Disconnection request read failed with error %s for PLC %s!", plc_tag_decode_error(rc), plc->key);

        /* check to see if we got the entire response back. */
        rc = plc->top_layer->process_response(plc->top_layer, plc->data, plc->data_capacity, &(data_start), &(data_end), &(plc->current_request_id));

        if(rc == PLCTAG_ERR_PARTIAL) {
            pdebug(DEBUG_DETAIL, "Partial response received, waiting for more data from PLC %s.", plc->key);

            NEXT_STATE(state_layer_disconnect_response_ready);
            rc = socket_callback_when_read_done(plc->socket, (void(*)(void*))plc_state_runner, plc, plc->data, plc->data_capacity, &(plc->payload_end));

            RESET_ON_ERROR(rc != PLCTAG_STATUS_OK, "Error %s setting up read complete callback for disconnect response for PLC %s!", plc_tag_decode_error(rc), plc->key);

            rc = PLCTAG_STATUS_OK;
            break;
        }

        RESET_ON_ERROR(rc != PLCTAG_STATUS_OK && rc != PLCTAG_STATUS_PENDING, "Error %s processing layer responses for disconnect response for PLC %s!", plc_tag_decode_error(rc), plc->key);

        if(rc == PLCTAG_STATUS_PENDING) {
            pdebug(DEBUG_INFO, "More layers require disconnect.");
            NEXT_STATE(state_build_layer_disconnect_request);
            rc = PLCTAG_STATUS_PENDING;
            break;
        } else {
            pdebug(DEBUG_INFO, "Disconnect done for PLC %s.", plc->key);
            plc->is_connected = false;
        }
    } while(0);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return rc;
}




/******** HELPER STATES *******/


int state_terminate(plc_p plc)
{
    pdebug(DEBUG_INFO, "Starting for PLC %s.", plc->key);

    pdebug(DEBUG_INFO, "Done for PLC %s.", plc->key);

    return PLCTAG_STATUS_OK;
}



/****** Module Support ******/


int plc_module_init(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    plc_list = NULL;

    rc = mutex_create(&plc_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, creating plc list mutex!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


void plc_module_teardown(void)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_INFO, "Starting.");

    if(plc_list != NULL) {
        pdebug(DEBUG_WARN, "PLC list not empty!");
    }

    plc_list = NULL;

    rc = mutex_destroy(&plc_list_mutex);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, destroying plc list mutex!", plc_tag_decode_error(rc));
        return;
    }

    pdebug(DEBUG_INFO, "Done.");
}



