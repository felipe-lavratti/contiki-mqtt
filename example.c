#include <contiki.h>
#include <mqtt-service.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#define PRINTF(...)
//#define PRINTF(...) printf(__VA_ARGS__)

PROCESS(mqtt_client_process, "Mqtt Client Process");

static uint8_t in_buffer[512]; /* This buffer limits the maximum size of a downward message */
static uint8_t out_buffer[128];

static bool ready;

process_event_t mqtt_event;
process_event_t event_mqtt_new_data;

void mqtt_client_init(void)
{
    mqtt_init(in_buffer, ARRAY_SIZE(in_buffer), out_buffer, ARRAY_SIZE(out_buffer));
    process_start(&mqtt_client_process, NULL);

    mqtt_event = process_alloc_event();
    event_mqtt_new_data = process_alloc_event();

    ready = false;
}

static uint16_t load_output_buffer(uint8_t * buf, uint16_t max_len)
{
    return (uint16_t)vfifo_pop_continue(&from_client, (char *) buf, max_len);
}

static void preconditions(process_data_t data)
{
    static int subscribe_state = 0;
    const char * topic;

    if (mqtt_event_is_connected(data))
    {
        subscribe_state = 1;
    }
    else if (mqtt_event_is_subscribe_failed(data))
    {
        PRINTF("mqtt_client: retrying subscribe.\n");
    }
    else if (mqtt_event_is_subscribed(data))
    {
        PRINTF("mqtt_client: subscribed successful.\n");
        subscribe_state ++;
    }
    else
    {
        goto skip;
    }


    switch (subscribe_state)
    {
    case 1:
        topic = "sub_topic_1";
        mqtt_subscribe(topic, 0);
        break;

    case 2:
        topic = "sub_topic_2";
        mqtt_subscribe(topic, 0);
        break;

    default:
        break;
    }

    skip:
    ready = subscribe_state == 3;
}

static uip_ipaddr_t * network_mqtt_server_address()
{
    static uip_ipaddr_t mqtt_server_addr = {{0,}};

    uip_ip6addr(&mqtt_server_addr, 0xaaaa, 0, 0, 0, 0, 0, 0, 1);

    return &mqtt_server_addr;
}

static uint16_t network_mqtt_server_port()
{
    return UIP_HTONS(1883);
}


PROCESS_THREAD(mqtt_client_process, ev, data)
{
    static int retry_pending = -1;
    static struct etimer et;
    static bool allowed_to_publish;

    static mqtt_connect_info_t connect_info =
    {
        .username = NULL,
        .password = NULL,
        .will_topic = NULL,
        .will_message = NULL,
        .keepalive_timeout = 60,
        .keepalive = 40,
        .retry_timeout = 5,
        .will_qos = 0,
        .will_retain = 0,
        .clean_session = 1
    };

    PROCESS_BEGIN();

    connect_info.client_id = "client_id";

    ready = false;
    allowed_to_publish = false;

    /* This timer is used to add a delay between the last inward publish and the first
     * outward publish. It is necessary because broker is configured max_inward=1, so
     * we will avoid lots of collision if we delay our upward data. */
    etimer_set(&et, CLOCK_SECOND / 4);
    etimer_stop(&et);

    mqtt_connect(network_mqtt_server_address(), network_mqtt_server_port(), 1, &connect_info);

    while (1)
    {
        int r = 0;

        PROCESS_WAIT_EVENT_UNTIL(ev == mqtt_event || ev == PROCESS_EVENT_TIMER || ev == event_mqtt_new_data);

        if (ev == mqtt_event)
        {
            preconditions(data);

            if (mqtt_event_is_connected(data))
            {
                PRINTF("Connected.\n");
                allowed_to_publish = true;
            }

            if (mqtt_event_is_receive_data(data))
            {
                static char topic[128];
                static char message[128];

                strncpy(topic, mqtt_event_get_topic(data), mqtt_event_get_topic_length(data));
                topic[mqtt_event_get_topic_length(data)] = 0;

                strncpy(message, mqtt_event_get_data(data), mqtt_event_get_data_length(data));
                message[mqtt_event_get_data_length(data)] = 0;

                allowed_to_publish = false;

                PRINTF("mqtt_client: Data received: %s, %s.\n", topic, message);
            }

            if (mqtt_event_is_receive_data_continuation(data))
            {
                const char* message = mqtt_event_get_data(data);
                PRINTF("mqtt_client:                %s.\n", message);
                r = vbuf_insert_continue(&to_client, message);
                if (r != 0)
                    be_throw_error_arg(ERR_RESPONSE_CODE, "%d", r);
            }

            if (mqtt_event_is_receive_end(data))
            {
                PRINTF("mqtt_client: receive end.\n");
                process_post_synch(&mqtt_demux_process, event_mqtt_new_data, &to_client);

                etimer_restart(&et);
                allowed_to_publish = true;
            }

            if (mqtt_event_is_published(data))
            {
                /* The undergoing upward publish has been published successful */
                PRINTF("mqtt_client: Published.\n");
                etimer_restart(&et);
                allowed_to_publish = true;
            }

            if (mqtt_event_is_publish_failed(data))
            {
                PRINTF("mqtt_client: Publish failed.\n");
                /* The undergoing upward publish has failed */
                retry_pending = mqtt_event_get_failed_id(data);
                etimer_restart(&et);
                allowed_to_publish = true;
            }
        }

        if (etimer_expired(&et) && allowed_to_publish && ready)
        {
            etimer_stop(&et);

            if (retry_pending != -1)
            {
                PRINTF("mqtt_client: Publishing retry.\n");

                r = mqtt_publish_mode2_retry(...);

                if (r == 0 || r == -2)
                    retry_pending = -1;

                allowed_to_publish = false;
            }
            else if (vfifo_n_of_msg(&from_client) > 0)
            {
                PRINTF("mqtt_client: Publishing.\n");

                r = mqtt_publish_mode2(...);

                allowed_to_publish = false;
            }
            else
            {
                PRINTF("mqtt_client: Nothing to publish.\n");
            }

            if (r == -2)
            {
                /* Invalid topic provided to publish or data is too big for the output buffer.
                 * Throw and error and discard the message.*/
                r = 0;
            }
        }
    }

    PROCESS_END();
}

bool mqtt_client_setup_ready(void)
{
    return ready;
}

int mqtt_client_write(...)
{
    vfifo_t * fifo;

    if (!initd)
        return -1;

    fifo = mqtt_client_get_outward_fifo();

    if (!fifo)
        return -1;

    if (!subjson)
        subjson = "";

    if (!enc_addr)
        return -1;

    if (strlen(enc_addr) == 0)
        enc_addr = buics_socket();

    /* I was using a buffer to enqueue writes, since the call bellow cannot
     * grant that the publish will happen, the proces has preconditions to
     * start a publish: a timer since last receive must timeout and there
     * must be no transmission or reception undergoing.
     */

    process_post_synch(&mqtt_client_process, event_mqtt_new_data, NULL);

    return 0;
}

