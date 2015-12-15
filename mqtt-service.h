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

/*
 * MQTT implementation for Contiki forked from: https://github.com/esar/contiki-mqtt
 *
 * Added features by Felipe Lavratti - felipe.lavratti@beyond.dm
 *
 * Has support for QOS 0, 1 and 2 with limitations:
 *
 * 1)
 *    This algorithm can be used freely with QOS=0 messages. If you want granted
 *    delivery you will naturally use QOS>0 on messaging and subscribing. You must know that
 *    this algorithm DOES NOT SUPPORT receiving two messages at the same time, for
 *    that reason the broker _must_ be configured with "maximum in-flight messages" to 1
 *    whenever using QOS>0.
 *
 * 2)
 *    This algorithm does not do buffering due to strong constrains of embedded platforms,
 *    so the way messages are received is affected. Client code receives data when a publish
 *    arrives, but the data cannot be processed by client code until it is notified that
 *    the handshake has finished. This goes the same way to QOS=0 messages.
 *
 *
 *
 */


#include "mqtt-msg.h"
#include "sys/process.h"
#include "net/ip/uip.h"

#include <stdbool.h>
#include <stdint.h>

#define MQTT_FLAG_RX_READY           0x1
#define MQTT_FLAG_TX_READY           0x2
#define MQTT_FLAG_CONNECTED          0x4
#define MQTT_FLAG_EXIT               0x8

#define MQTT_EVENT_TYPE_NONE                       0
#define MQTT_EVENT_TYPE_CONNECTED                  1
#define MQTT_EVENT_TYPE_DISCONNECTED               2
#define MQTT_EVENT_TYPE_SUBSCRIBED                 3
#define MQTT_EVENT_TYPE_UNSUBSCRIBED               4
#define MQTT_EVENT_TYPE_RECEIVE_DATA               5
#define MQTT_EVENT_TYPE_PUBLISHED                  6
#define MQTT_EVENT_TYPE_EXITED                     7
#define MQTT_EVENT_TYPE_RECEIVE_DATA_CONTINUATION  8
#define MQTT_EVENT_TYPE_RECEIVE_END                9
#define MQTT_EVENT_TYPE_PUBLISH_FAILED             10
#define MQTT_EVENT_TYPE_SUBSCRIBE_FAILED           11
#define MQTT_EVENT_TYPE_UNSUBSCRIBE_FAILED         12

typedef struct mqtt_event_data_t
{
  uint8_t type;
  const char* topic;
  const char* data;
  uint16_t topic_length;
  uint16_t data_length;
  uint16_t data_offset;
  uint16_t failed_id;

} mqtt_event_data_t;

extern int mqtt_flags;
extern process_event_t mqtt_event;


// Must be called before any other function is  called.
//
// Initialises the MQTT client library and associates the
// provided buffers with it. The buffer memory must remain
// valid throughout the use of the API.
void mqtt_init(uint8_t* in_buffer, int in_buffer_length,
               uint8_t* out_buffer, int out_buffer_length);

// Starts an asynchronous connect to the server specified by
// the address and port. If auto_reconnect is non-zero then
// the it will keep trying to connect indefinitely and if the
// connection drops it will attempt to reconnect.
// The info structure provides other connection details
// such as username/password, will topic/message, etc.
// The memory pointed to by info must remain valid
// throughout the use of the API.
//
// The calling process will receive an mqtt_event of type
// MQTT_EVENT_CONNECTED when the operation is complete.
// Or an event of type MQTT_EVENT_DISCONNECTED if the
// connect attempt fails.
int mqtt_connect(uip_ip6addr_t* address, uint16_t port,
                 int auto_reconnect, mqtt_connect_info_t* info);

// Starts an asynchronous disconnect from the server.
// The calling process will receive a mqtt_event of type
// MQTT_EVENT_TYPE_EXITED when the operation is complete.
int mqtt_disconnect(void);

// Starts an asynchronous subscribe to the specified topic
// The calling process will receive a mqtt_event of type
// MQTT_EVENT_TYPE_SUBSCRIBE when the servers reply has
// been received.
int mqtt_subscribe(const char* topic, int qos);

// Starts an asynchronous unsubscribe of the specified topic.
// The calling process will receive a mqtt_event of type
// MQTT_EVENT_TYPE_UNSUBSCRIBED when the server's reply
// has been received.
int mqtt_unsubscribe(const char* topic);

int mqtt_publish(const char* topic, const char* data, int qos, int retain);
int mqtt_publish_retry(const char* topic, const char* data, int qos, int retain, uint16_t failed_id);
int mqtt_publish_mode2(const char* topic, int qos, int retain, mqtt_mode2_callback_t buffer_loader, uint16_t expected_len);
int mqtt_publish_mode2_retry(const char* topic, int qos, int retain, uint16_t failed_id, mqtt_mode2_callback_t buffer_loader, uint16_t expected_len);

static inline int mqtt_connected(void)
{
  return (mqtt_flags & MQTT_FLAG_CONNECTED);
}

static inline bool mqtt_ready(void)
{
    if (!(mqtt_flags & MQTT_FLAG_RX_READY))
        return false;

    if (!(mqtt_flags & MQTT_FLAG_TX_READY))
        return false;

    return true;
}

static inline int mqtt_event_is_connected(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_CONNECTED;
}
static inline int mqtt_event_is_disconnected(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_DISCONNECTED;
}
static inline int mqtt_event_is_subscribed(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_SUBSCRIBED;
}
static inline int mqtt_event_is_unsubscribed(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_UNSUBSCRIBED;
}
static inline int mqtt_event_is_receive_data(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_RECEIVE_DATA;
}
static inline int mqtt_event_is_published(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_PUBLISHED;
}
static inline int mqtt_event_is_exited(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_EXITED;
}
static inline int mqtt_event_is_receive_data_continuation(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_RECEIVE_DATA_CONTINUATION;
}
static inline int mqtt_event_is_receive_end(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_RECEIVE_END;
}
static inline int mqtt_event_is_publish_failed(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_PUBLISH_FAILED;
}
static inline int mqtt_event_is_subscribe_failed(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_SUBSCRIBE_FAILED;
}
static inline int mqtt_event_is_unsubscribe_failed(void* data)
{
  return ((mqtt_event_data_t*)data)->type == MQTT_EVENT_TYPE_UNSUBSCRIBE_FAILED;
}

static inline const char* mqtt_event_get_topic(void* data)
{
  return ((mqtt_event_data_t*)data)->topic;
}
static inline uint16_t mqtt_event_get_topic_length(void* data)
{
  return ((mqtt_event_data_t*)data)->topic_length;
}
static inline const char* mqtt_event_get_data(void* data)
{
  return ((mqtt_event_data_t*)data)->data;
}
static inline uint16_t mqtt_event_get_data_length(void* data)
{
  return ((mqtt_event_data_t*)data)->data_length;
}
static inline uint16_t mqtt_event_get_data_offset(void* data)
{
  return ((mqtt_event_data_t*)data)->data_offset;
}
static inline uint16_t mqtt_event_get_failed_id(void* data)
{
  return ((mqtt_event_data_t*)data)->failed_id;
}


