/*
 * Copyright (c) 2014, Stephen Robinson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 *  are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "mqtt-service.h"

#include "contiki.h"
#include "contiki-net.h"
#include "sys/etimer.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

//#define DEBUG

#ifdef DEBUG

#define PRINTF printf
#define DBG_PRINTF printf
#define DBG_MQTT_MSG(__pre, __msg, __mid) ({int m = ((__msg)-1) % 14; DBG_PRINTF(__pre"%s:%d\n", mqtt_message_type_name(m), __mid); })

const char * const _mqtt_message_type_name[] =
{ "CONNECT    ", "CONNACK    ", "PUBLISH    ", "PUBACK     ", "PUBREC     ", "PUBREL     ",
    "PUBCOMP    ", "SUBSCRIBE  ", "SUBACK     ", "UNSUBSCRIBE", "UNSUBACK   ", "PINGREQ    ",
    "PINGRESP   ", "DISCONNECT ", };

const char * mqtt_message_type_name(int m)
{
    if (m < 0 || m > 13)
        return "unknown";

    return _mqtt_message_type_name[m];
}

#else /* DEBUG */
#define PRINTF(...)
#define DBG_PRINTF
#define DBG_MQTT_MSG(__pre, __msg, __mid)

#endif /* DEBUG */

typedef struct mqtt_state_t
{
    uip_ip6addr_t address;
    uint16_t port;
    int auto_reconnect;
    mqtt_connect_info_t* connect_info;

    struct process* calling_process;
    struct uip_conn* tcp_connection;

    struct psock ps;
    uint8_t* in_buffer;
    uint8_t* out_buffer;
    int in_buffer_length;
    int out_buffer_length;
    uint16_t message_length;
    uint16_t message_length_read;
    mqtt_message_t* outbound_message;
    mqtt_connection_t mqtt_connection;
    int pending_msg_type;
    int upward_pending_msg_type;
    uint8_t upward_pending_msg_qos;
    uint16_t upward_pending_msg_id;
    bool set_upward_timer;

    uint8_t tcp_buffer[128];
} mqtt_state_t;

PROCESS(mqtt_process, "MQTT Process");

process_event_t mqtt_event;
mqtt_state_t mqtt_state;
int mqtt_flags = 0;

/*********************************************************************
 *
 *    Public API
 *
 *********************************************************************/

// Initialise the MQTT client, must be called before anything else
void mqtt_init(uint8_t* in_buffer, int in_buffer_length, uint8_t* out_buffer, int out_buffer_length)
{
    mqtt_event = process_alloc_event();

    mqtt_state.in_buffer = in_buffer;
    mqtt_state.in_buffer_length = in_buffer_length;
    mqtt_state.out_buffer = out_buffer;
    mqtt_state.out_buffer_length = out_buffer_length;
}

// Connect to the specified server
int mqtt_connect(uip_ip6addr_t* address, uint16_t port, int auto_reconnect,
            mqtt_connect_info_t* info)
{
    if (process_is_running(&mqtt_process))
        return -1;

    mqtt_state.address = *address;
    mqtt_state.port = port;
    mqtt_state.auto_reconnect = auto_reconnect;
    mqtt_state.connect_info = info;
    mqtt_state.calling_process = PROCESS_CURRENT();
    process_start(&mqtt_process, (char*) &mqtt_state);

    return 0;
}

// Disconnect from the server
int mqtt_disconnect()
{
    if (!process_is_running(&mqtt_process))
        return -1;

    PRINTF("mqtt: exiting...\n");
    mqtt_flags &= ~(MQTT_FLAG_TX_READY | MQTT_FLAG_RX_READY);
    mqtt_flags &= ~MQTT_FLAG_CONNECTED;
    mqtt_flags |= MQTT_FLAG_EXIT;
    tcpip_poll_tcp(mqtt_state.tcp_connection);

    return 0;
}

// Subscribe to the specified topic
int mqtt_subscribe(const char* topic, int qos)
{
    if (!mqtt_ready())
        return -1;

    PRINTF("mqtt: sending subscribe...\n");
    mqtt_state.outbound_message = mqtt_msg_subscribe(&mqtt_state.mqtt_connection, topic, qos,
                &mqtt_state.upward_pending_msg_id);
    mqtt_flags &= ~MQTT_FLAG_TX_READY;
    mqtt_state.upward_pending_msg_type = MQTT_MSG_TYPE_SUBSCRIBE;
    mqtt_state.set_upward_timer = true;
    tcpip_poll_tcp(mqtt_state.tcp_connection);

    return 0;
}

int mqtt_unsubscribe(const char* topic)
{
    if (!mqtt_ready())
        return -1;

    PRINTF("mqtt: sending unsubscribe\n");
    mqtt_state.outbound_message = mqtt_msg_unsubscribe(&mqtt_state.mqtt_connection, topic,
                &mqtt_state.upward_pending_msg_id);
    mqtt_flags &= ~MQTT_FLAG_TX_READY;
    mqtt_state.upward_pending_msg_type = MQTT_MSG_TYPE_UNSUBSCRIBE;
    mqtt_state.set_upward_timer = true;
    tcpip_poll_tcp(mqtt_state.tcp_connection);

    return 0;
}

// Publish the specified message
static int publish(const char* topic, const char* data, int data_length, int qos, int retain,
            uint16_t id)
{
    mqtt_message_t * m;

    if (!mqtt_ready())
        return -1;

    mqtt_state.upward_pending_msg_id = id;
    m = mqtt_msg_publish(&mqtt_state.mqtt_connection, topic, data, data_length, qos, retain,
                &mqtt_state.upward_pending_msg_id);

    if (m->length <= 0)
        return -2;

    PRINTF("mqtt: sending publish...\n");
    mqtt_state.upward_pending_msg_qos = qos;
    mqtt_state.outbound_message = m;
    mqtt_flags &= ~MQTT_FLAG_TX_READY;
    mqtt_state.upward_pending_msg_type = MQTT_MSG_TYPE_PUBLISH;
    mqtt_state.set_upward_timer = true;
    tcpip_poll_tcp(mqtt_state.tcp_connection);

    return 0;
}

// Publish the specified message
static int publish_mode2(const char* topic, int qos, int retain, uint16_t id,
            mqtt_mode2_callback_t loader, uint16_t expected_len)
{
    mqtt_message_t * m;

    if (!mqtt_ready())
        return -1;

    PRINTF("mqtt: sending publish...\n");
    mqtt_state.upward_pending_msg_id = id;
    m = mqtt_msg_publish_mode2(&mqtt_state.mqtt_connection, topic, loader, expected_len, qos,
                retain, &mqtt_state.upward_pending_msg_id);

    if (m->length <= 0)
        return -2;

    mqtt_state.upward_pending_msg_qos = qos;
    mqtt_state.outbound_message = m;
    mqtt_flags &= ~MQTT_FLAG_TX_READY;
    mqtt_state.upward_pending_msg_type = MQTT_MSG_TYPE_PUBLISH;
    mqtt_state.set_upward_timer = true;
    tcpip_poll_tcp(mqtt_state.tcp_connection);

    return 0;
}

int mqtt_publish(const char* topic, const char* data, int qos, int retain)
{
    return publish(topic, data, data != NULL ? strlen(data) : 0, qos, retain, 0);
}

int mqtt_publish_retry(const char* topic, const char* data, int qos, int retain, uint16_t failed_id)
{
    return publish(topic, data, data != NULL ? strlen(data) : 0, qos, retain, failed_id);
}

int mqtt_publish_mode2(const char* topic, int qos, int retain, mqtt_mode2_callback_t buffer_loader,
            uint16_t expected_len)
{
    return publish_mode2(topic, qos, retain, 0, buffer_loader, expected_len);
}

int mqtt_publish_mode2_retry(const char* topic, int qos, int retain, uint16_t failed_id,
            mqtt_mode2_callback_t buffer_loader, uint16_t expected_len)
{
    return publish_mode2(topic, qos, retain, failed_id, buffer_loader, expected_len);
}

/***************************************************************
 *
 *    Internals
 *
 ***************************************************************/

static void complete_pending(mqtt_state_t* state, int event_type, int flag_to_set)
{
    mqtt_event_data_t event_data;

    mqtt_flags |= flag_to_set;
    event_data.type = event_type;
    process_post_synch(state->calling_process, mqtt_event, &event_data);
}

static void fail_pending(mqtt_state_t* state, int event_type, int flag_to_set, uint16_t failed_id)
{
    mqtt_event_data_t event_data;

    mqtt_flags |= flag_to_set;
    event_data.type = event_type;
    event_data.failed_id = failed_id;
    process_post_synch(state->calling_process, mqtt_event, &event_data);
}

static void deliver_publish(mqtt_state_t* state, const uint8_t* message, int length)
{
    mqtt_event_data_t event_data;

    event_data.type = MQTT_EVENT_TYPE_RECEIVE_DATA;

    event_data.topic_length = length;
    event_data.topic = mqtt_get_publish_topic(message, &event_data.topic_length);

    event_data.data_length = length;
    event_data.data = mqtt_get_publish_data(message, &event_data.data_length);

    process_post_synch(state->calling_process, mqtt_event, &event_data);
}

static PT_THREAD(handle_mqtt_connection(mqtt_state_t* state))
{
    static struct etimer keepalive_timer;
    static struct etimer upward_timer;

    static uint8_t msg_qos;
    static uint8_t msg_type;
    static uint16_t msg_id;
    static int last_completed_id = -1;
    static int current_downward_id = -1;

    PSOCK_BEGIN(&state->ps)
    ;

    // Initialise and send CONNECT message
    mqtt_msg_init(&state->mqtt_connection, state->out_buffer, state->out_buffer_length);
    state->outbound_message = mqtt_msg_connect(&state->mqtt_connection, state->connect_info);

    PRINTF("mqtt: sending connect.\n");
    PSOCK_SEND(&state->ps, state->outbound_message->data, state->outbound_message->length);
    state->outbound_message = NULL;

    // Wait for CONACK message
    PSOCK_READBUF_LEN(&state->ps, 2);
    PRINTF("psock read %d\n", PSOCK_DATALEN(&state->ps));
    if (mqtt_get_type(state->tcp_buffer) != MQTT_MSG_TYPE_CONNACK)
    {
        PRINTF("mqtt: sock closed on connection.\n");
        PSOCK_CLOSE_EXIT(&state->ps);
    }
    // Tell the client we're connected and should resubscribe if needed.
    mqtt_flags |= MQTT_FLAG_CONNECTED;
    PRINTF("mqtt: sock connected\n");
    state->pending_msg_type = 0;
    complete_pending(state, MQTT_EVENT_TYPE_CONNECTED, (MQTT_FLAG_TX_READY | MQTT_FLAG_RX_READY));

    // Setup the keep alive timer and enter main message processing loop
    etimer_set(&keepalive_timer, CLOCK_SECOND * state->connect_info->keepalive);
    etimer_set(&upward_timer, CLOCK_SECOND * state->connect_info->retry_timeout);
    while (1)
    {
        PRINTF("mqtt: psock waiting\n");

        PSOCK_WAIT_UNTIL(&state->ps,
                         PSOCK_NEWDATA(&state->ps) ||
                         state->outbound_message != NULL ||
                         (etimer_expired(&upward_timer) && state->upward_pending_msg_type != 0) ||
                         etimer_expired(&keepalive_timer)
                        );

        // If there's a new message waiting to go out, then send it
        if (state->outbound_message != NULL)
        {
            PRINTF("mqtt: psock woke: outbound %d\n", state->outbound_message->length);
            PSOCK_SEND(&state->ps, state->outbound_message->data, state->outbound_message->length);
            DBG_MQTT_MSG("          tx:", mqtt_get_type(state->outbound_message->data),
                        mqtt_get_id(state->outbound_message->data,
                                    state->outbound_message->length));
            state->outbound_message = NULL;

            if (state->set_upward_timer)
            {
                etimer_restart(&upward_timer);
                mqtt_state.set_upward_timer = false;
            }

            // If it was a PUBLISH message with QoS-0 then tell the client it's done
            if (state->upward_pending_msg_type == MQTT_MSG_TYPE_PUBLISH
                        && state->upward_pending_msg_qos == 0)
            {
                state->upward_pending_msg_type = 0;
                complete_pending(state, MQTT_EVENT_TYPE_PUBLISHED, MQTT_FLAG_TX_READY);
            }

            // If it was a PUBACK message then tell the client it's done
            if (state->pending_msg_type == MQTT_MSG_TYPE_PUBACK)
            {
                state->pending_msg_type = 0;
                if (last_completed_id != current_downward_id)
                {
                    /* Avoid duplicate receive_end event when re-sending PUBCOMP due to network loss */
                    complete_pending(state, MQTT_EVENT_TYPE_RECEIVE_END, MQTT_FLAG_RX_READY);
                    last_completed_id = current_downward_id;
                }
            }

            // If it was a PUBCOMP message then tell the client it's done
            if (state->pending_msg_type == MQTT_MSG_TYPE_PUBCOMP)
            {
                state->pending_msg_type = 0;
                if (last_completed_id != current_downward_id)
                {
                    /* Avoid duplicate receive_end event when re-sending PUBCOMP due to network loss */
                    complete_pending(state, MQTT_EVENT_TYPE_RECEIVE_END, MQTT_FLAG_RX_READY);
                    last_completed_id = current_downward_id;
                }
            }

            // Reset the keepalive timer as we've just sent some data
            etimer_restart(&keepalive_timer);
            continue;
        }

        // If the keep-alive timer expired then prepare a ping for sending
        // and reset the timer
        if (etimer_expired(&keepalive_timer))
        {
            PRINTF("mqtt: psock woke: keepalive \n");
            state->outbound_message = mqtt_msg_pingreq(&state->mqtt_connection);
            etimer_set(&keepalive_timer, CLOCK_SECOND * state->connect_info->retry_timeout);
            continue;
        }

        /* Upward operation timeouted */
        if (etimer_expired(&upward_timer) && state->upward_pending_msg_type != 0)
        {
            PRINTF("mqtt: psock woke: upward timer\n");

            if (state->upward_pending_msg_type == MQTT_MSG_TYPE_SUBSCRIBE)
            {
                state->upward_pending_msg_type = 0;
                /* Fail a subscribe to the client */
                fail_pending(state, MQTT_EVENT_TYPE_SUBSCRIBE_FAILED, MQTT_FLAG_TX_READY,
                            state->upward_pending_msg_id);
            }
            else if (state->upward_pending_msg_type == MQTT_MSG_TYPE_UNSUBSCRIBE)
            {
                state->upward_pending_msg_type = 0;
                /* Fail a unsubscribe to the client */
                fail_pending(state, MQTT_EVENT_TYPE_UNSUBSCRIBE_FAILED, MQTT_FLAG_TX_READY,
                            state->upward_pending_msg_id);
            }
            else if (state->upward_pending_msg_type == MQTT_MSG_TYPE_PUBLISH)
            {
                state->upward_pending_msg_type = 0;
                /* Fail a publish to the client */
                fail_pending(state, MQTT_EVENT_TYPE_PUBLISH_FAILED, MQTT_FLAG_TX_READY,
                            state->upward_pending_msg_id);
            }
            else if (state->upward_pending_msg_type == MQTT_MSG_TYPE_PUBREL)
            {
                /* PUBREC received by the broker, can'fail publish anymore, retry PUBREL. */
                state->outbound_message = mqtt_msg_pubrel(&state->mqtt_connection,
                            state->upward_pending_msg_id);
                etimer_restart(&upward_timer);
            }
            continue;
        }

        // If we get here we must have woken for new incoming data,
        // read and process it.
        PRINTF("mqtt: psock woke: psock received\n");
        state->message_length_read = 0;
        PSOCK_READBUF_LEN(&state->ps, 2);
        PRINTF("psock read %d\n", PSOCK_DATALEN(&state->ps));
        memcpy(state->in_buffer, state->tcp_buffer, PSOCK_DATALEN(&state->ps));
        state->message_length_read = PSOCK_DATALEN(&state->ps);

        again:
        state->message_length = mqtt_get_total_length(state->in_buffer, state->message_length_read);

        PRINTF("mqtt: read %d of %d\n", state->message_length_read, state->message_length);

        while (state->message_length_read < state->message_length)
        {
            PSOCK_READBUF_LEN(&state->ps, state->message_length - state->message_length_read);
            PRINTF("psock read %d\n", PSOCK_DATALEN(&state->ps));
            memcpy(state->in_buffer + state->message_length_read, state->tcp_buffer, PSOCK_DATALEN(&state->ps));
            state->message_length_read += PSOCK_DATALEN(&state->ps);
            PRINTF("mqtt: read %d of %d\n", state->message_length_read, state->message_length);
        }

        // Reset the keepalive timer as we've just received some data
        etimer_restart(&keepalive_timer);

        msg_type = mqtt_get_type(state->in_buffer);
        msg_qos = mqtt_get_qos(state->in_buffer);
        msg_id = mqtt_get_id(state->in_buffer, state->in_buffer_length);
        DBG_MQTT_MSG("          rx:", msg_type, msg_id);
        switch (msg_type)
        {
        /* UPWARD flux */
        /* QOS 1 */
        case MQTT_MSG_TYPE_SUBACK:
            if (state->upward_pending_msg_type == MQTT_MSG_TYPE_SUBSCRIBE
                        && state->upward_pending_msg_id == msg_id)
            {
                state->upward_pending_msg_type = 0;
                complete_pending(state, MQTT_EVENT_TYPE_SUBSCRIBED, MQTT_FLAG_TX_READY);
            }
            break;
            /* QOS 1 */
        case MQTT_MSG_TYPE_UNSUBACK:
            if (state->upward_pending_msg_type == MQTT_MSG_TYPE_UNSUBSCRIBE
                        && state->upward_pending_msg_id == msg_id)
            {
                state->upward_pending_msg_type = 0;
                complete_pending(state, MQTT_EVENT_TYPE_UNSUBSCRIBED, MQTT_FLAG_TX_READY);
            }
            break;
            /* QOS 1 */
        case MQTT_MSG_TYPE_PUBACK:
            if (state->upward_pending_msg_type == MQTT_MSG_TYPE_PUBLISH
                        && state->upward_pending_msg_id == msg_id)
            {
                state->upward_pending_msg_type = 0;
                complete_pending(state, MQTT_EVENT_TYPE_PUBLISHED, MQTT_FLAG_TX_READY);
            }
            break;
            /* QOS 2 */
        case MQTT_MSG_TYPE_PUBREC:
            state->outbound_message = mqtt_msg_pubrel(&state->mqtt_connection, msg_id);
            state->upward_pending_msg_type = MQTT_MSG_TYPE_PUBREL;
            etimer_restart(&upward_timer);
            break;
        case MQTT_MSG_TYPE_PUBCOMP:
            if (state->upward_pending_msg_type == MQTT_MSG_TYPE_PUBREL
                        && state->upward_pending_msg_id == msg_id)
            {
                state->upward_pending_msg_type = 0;
                complete_pending(state, MQTT_EVENT_TYPE_PUBLISHED, MQTT_FLAG_TX_READY);
            }
            break;

            /* DOWNARD flux */
        case MQTT_MSG_TYPE_PUBLISH:
            mqtt_flags &= ~MQTT_FLAG_RX_READY;
            current_downward_id = msg_id;
            if (msg_qos == 1)
            {
                state->pending_msg_type = MQTT_MSG_TYPE_PUBACK;
                state->outbound_message = mqtt_msg_puback(&state->mqtt_connection, msg_id);
            }
            else if (msg_qos == 2)
            {
                state->pending_msg_type = MQTT_MSG_TYPE_PUBREC;
                state->outbound_message = mqtt_msg_pubrec(&state->mqtt_connection, msg_id);
            }
            break;
        case MQTT_MSG_TYPE_PUBREL:
            state->pending_msg_type = MQTT_MSG_TYPE_PUBCOMP;
            state->outbound_message = mqtt_msg_pubcomp(&state->mqtt_connection, msg_id);
            break;

            /* PING flux */
        case MQTT_MSG_TYPE_PINGREQ:
            state->outbound_message = mqtt_msg_pingresp(&state->mqtt_connection);
            break;
        case MQTT_MSG_TYPE_PINGRESP:
            etimer_set(&keepalive_timer, CLOCK_SECOND * state->connect_info->keepalive);
            break;
        }

        if (msg_type == MQTT_MSG_TYPE_PUBLISH)
        {
            PRINTF("mqtt: dealing with PUBLISH\n");
            deliver_publish(state, state->in_buffer, state->message_length_read);

            if (msg_qos == 0)
            {
                PRINTF("mqtt: completing.\n");
                state->pending_msg_type = 0;
                complete_pending(state, MQTT_EVENT_TYPE_RECEIVE_END, MQTT_FLAG_RX_READY);
            }

            if (state->message_length_read > state->message_length)
            {
                PRINTF("mqtt: psock woke: processing next in the buffer\n");
                memmove(state->in_buffer,
                        state->in_buffer + state->message_length,
                        state->message_length_read - state->message_length);
                state->message_length_read -= state->message_length;
                PRINTF("mqtt: rewinded buffer in %d bytes\n", state->message_length);

                /* make sure it has at least two bytes loaded */
                if (state->message_length_read == 1)
                {
                    PRINTF("mqtt: loading one more byte\n");
                    PSOCK_READBUF_LEN(&state->ps, 1);
                    PRINTF("psock read %d\n", PSOCK_DATALEN(&state->ps));
                    memcpy(state->in_buffer + state->message_length_read, state->tcp_buffer, PSOCK_DATALEN(&state->ps));
                    state->message_length_read += PSOCK_DATALEN(&state->ps);
                }

                PRINTF("mqtt: processing next packet with %d bytes read\n", state->message_length_read);
                PRINTF("mqtt: next packet word: %X %X\n", state->in_buffer[0], state->in_buffer[1]);
                goto again;
            }
        }
    }

PSOCK_END(&state->ps);
}

PROCESS_THREAD(mqtt_process, ev, data)
{
mqtt_event_data_t event_data;
static struct etimer et;

PROCESS_BEGIN()
;

while (1)
{
    mqtt_state.tcp_connection = tcp_connect(&mqtt_state.address, mqtt_state.port, NULL);

    wait_again:
    PROCESS_WAIT_EVENT_UNTIL(ev == tcpip_event);

    if (uip_conn != mqtt_state.tcp_connection)
    {
        uip_abort();
        goto wait_again;
    }

    if (!uip_connected())
    {
        etimer_set(&et, CLOCK_SECOND * 5);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

        PRINTF("mqtt: connect failed\n");
        continue;
    }
    else
    {
        PRINTF("mqtt: connected\n");
    }

    // reserve one byte at the end of the buffer so there's space to NULL terminate
    PSOCK_INIT(&mqtt_state.ps, mqtt_state.tcp_buffer, 127);

    handle_mqtt_connection(&mqtt_state);

    while (1)
    {
        PROCESS_WAIT_EVENT_UNTIL(ev == tcpip_event && uip_conn == mqtt_state.tcp_connection);

        if (mqtt_flags & MQTT_FLAG_EXIT)
        {
            uip_close();

            event_data.type = MQTT_EVENT_TYPE_EXITED;
            process_post_synch(mqtt_state.calling_process, mqtt_event, &event_data);
            PROCESS_EXIT()
            ;
        }

        if (uip_aborted() || uip_timedout() || uip_closed())
        {
            mqtt_flags &= ~MQTT_FLAG_CONNECTED;
            event_data.type = MQTT_EVENT_TYPE_DISCONNECTED;
            process_post_synch(mqtt_state.calling_process, mqtt_event, &event_data);
            PRINTF("mqtt: lost connection: %s\n", uip_aborted() ? "aborted" :
            uip_timedout() ? "timed out" :
                                                  uip_closed() ? "closed" : "unknown");
            break;
        }
        else
            handle_mqtt_connection(&mqtt_state);
    }

    if (!mqtt_state.auto_reconnect)
        break;
}

event_data.type = MQTT_EVENT_TYPE_EXITED;
process_post_synch(mqtt_state.calling_process, mqtt_event, &event_data);

PROCESS_END();
}
