#ifndef MQTT_H
#define MQTT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

#ifdef USE_MOSQUITTO
#include <mosquitto.h>
#else
#include <wolfmqtt/mqtt_client.h>
#include <wolfmqtt/mqtt_packet.h>
#include <wolfmqtt/mqtt_socket.h>
#include <wolfmqtt/mqtt_types.h>
#endif

#include <mqttv4.h>

#define EMPTY_TOPIC         "EMPTY_TOPIC"
#define MQTT_MAX_PACKET_SZ  1024
#define MQTT_PING_INTERVAL  30
#define RECONNECT_DELAY     3000
#define DEFAULT_CON_TIMEOUT 5000
#define DEFAULT_CMD_TIMEOUT 30000

typedef struct
{
    int         enable;
    char        client_id[64];
    char       *user;
    char       *password;
    char        host[128];
    int         port;
    int         tls;
    int         keepalive;
    char        bind_address[32];
    int         qos;
    int         retain_birth_will;
    int         retain_motion;
    int         retain_motion_image;
    int         retain_motion_files;
    int         retain_sound_detection;

    char       *mqtt_prefix;
    char       *topic_birth_will;
    char       *birth_msg;
    char       *will_msg;
} mqtt_conf_t;

typedef struct
{
    char* topic;
    char* msg;
    int len;
} mqtt_msg_t;

int init_mqtt(void);
void stop_mqtt(void);

void mqtt_loop(void);

void mqtt_init_conf(mqtt_conf_t *conf);
void mqtt_set_conf(mqtt_conf_t *conf);
void mqtt_check_connection();
int mqtt_send_message(mqtt_msg_t *msg, int retain);

int mqtt_connect();

#endif // MQTT_H
