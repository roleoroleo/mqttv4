#ifndef USE_MOSQUITTO
#include "mqtt.h"
#include <pthread.h>

extern volatile sig_atomic_t run;

static MqttNet wolf_net;
static MqttClient wolf_client;
static mqtt_conf_t *mqtt_conf;

unsigned char tx_buf[MQTT_MAX_PACKET_SZ];
unsigned char rx_buf[MQTT_MAX_PACKET_SZ];

static int packet_id_last = 0;

enum conn_states {CONN_DISCONNECTED, CONN_CONNECTING, CONN_CONNECTED};
static enum conn_states conn_state;

static int init_wolf_instance();
static int mqtt_tls_cb(MqttClient* client);
static int mqtt_message_cb(MqttClient *client, MqttMessage *msg,
                           byte msg_new, byte msg_done);

static time_t last_ping = 0;

static pthread_mutex_t wolf_mtx;

/*******************************************************************************
*
* WolfMqtt functions
*
*******************************************************************************/

struct net_ctx {
    int sockfd;
};
struct net_ctx ctx;

static int net_connect(void *context, const char *host, word16 port, int timeout_ms)
{
    (void)timeout_ms;
    struct net_ctx *ctx = (struct net_ctx *) context;
    struct addrinfo hints, *res = NULL;
    char portstr[8];

    snprintf(portstr, sizeof(portstr), "%u", (unsigned) port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        fprintf(stderr, "Error in getaddrinfo\n");
        return -1;
    }

    ctx->sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (ctx->sockfd < 0) {
        fprintf(stderr, "Error in socket\n");
        freeaddrinfo(res);
        return -2;
    }

    if (connect(ctx->sockfd, res->ai_addr, res->ai_addrlen) != 0) {
        fprintf(stderr,  "Error in connect\n");
        close(ctx->sockfd);
        ctx->sockfd = -1;
        freeaddrinfo(res);
        return -3;
    }

    freeaddrinfo(res);

    return MQTT_CODE_SUCCESS;
}

static int net_write(void *context, const byte *buf, int buf_len, int timeout_ms)
{
    (void) timeout_ms;
    struct net_ctx *ctx = (struct net_ctx *) context;
    int sent = send(ctx->sockfd, buf, buf_len, 0);
    if (sent < 0) {
        fprintf(stderr, "Error in send\n");
    }

    return sent;
}


static int net_read(void *context, byte *buf, int buf_len, int timeout_ms)
{
    struct net_ctx *ctx = (struct net_ctx *) context;
    struct timeval tv_old, tv_new;
    socklen_t tv_old_size = sizeof(tv_old);
    int have_old = 0;
    if (getsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, (struct timeval *) &tv_old, &tv_old_size) == 0)
        have_old = 1;
    tv_new.tv_sec = timeout_ms / 1000;
    tv_new.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, (struct timeval *) &tv_new, sizeof(tv_new));
    int r = recv(ctx->sockfd, buf, buf_len, 0);
    int e = errno;
    if (r < 0) {
        if (e == EAGAIN) {
            r = MQTT_CODE_ERROR_TIMEOUT;
        } else {
            // 2nd try
            if (e == EINTR) {
                r = recv(ctx->sockfd, buf, buf_len, 0);
                e = errno;
                if (r < 0) {
                    if (e == EAGAIN) {
                        r = MQTT_CODE_ERROR_TIMEOUT;
                    } else {
                        // 3rd try
                        if (e == EINTR) {
                            r = recv(ctx->sockfd, buf, buf_len, 0);
                            e = errno;
                            if (r < 0) {
                                if (e == EAGAIN) {
                                    r = MQTT_CODE_ERROR_TIMEOUT;
                                } else {
                                    fprintf(stderr, "Error in recv %d (%d)\n", r, e);
                                }
                            }
                        } else {
                            fprintf(stderr, "Error in recv %d (%d)\n", r, e);
                        }
                    }
                }
            } else {
                fprintf(stderr, "Error in recv %d (%d)\n", r, e);
            }
        }
    }
    if (have_old)
        setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, (struct timeval *) &tv_old, sizeof(tv_old));

    return r;
}

static int net_disconnect(void *context)
{
    struct net_ctx *ctx = (struct net_ctx *) context;
    if (ctx->sockfd >= 0) close(ctx->sockfd);
    ctx->sockfd = -1;

    return MQTT_CODE_SUCCESS;
}

static word16 mqtt_get_packetid(void)
{
    /* Check rollover */
    if (packet_id_last >= MAX_PACKET_ID) {
        packet_id_last = 0;
    }

    return ++packet_id_last;
}

void send_birth_msg()
{
    char topic[128];
    mqtt_msg_t msg;

    msg.msg=mqtt_conf->birth_msg;
    msg.len=strlen(msg.msg);
    msg.topic=topic;

    snprintf(topic, sizeof(topic), "%s/%s", mqtt_conf->mqtt_prefix, mqtt_conf->topic_birth_will);

    mqtt_send_message(&msg, mqtt_conf->retain_birth_will);
}

void send_will_msg()
{
    char topic[128];
    mqtt_msg_t msg;

    msg.msg=mqtt_conf->will_msg;
    msg.len=strlen(msg.msg);
    msg.topic=topic;

    snprintf(topic, sizeof(topic), "%s/%s", mqtt_conf->mqtt_prefix, mqtt_conf->topic_birth_will);

    mqtt_send_message(&msg, mqtt_conf->retain_birth_will);
}

int init_mqtt(void)
{
    int rc;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&wolf_mtx, &attr);
    pthread_mutexattr_destroy(&attr);

    rc = init_wolf_instance();
    if(rc != 0)
    {
        fprintf(stderr, "Can't create wolf mqtt instance.\n");
        return -2;
    }

    conn_state = CONN_DISCONNECTED;

    return 0;
}

void stop_mqtt(void)
{
    pthread_mutex_lock(&wolf_mtx);

    send_will_msg();

    MqttClient_Disconnect(&wolf_client);
    MqttClient_NetDisconnect(&wolf_client);
    MqttClient_DeInit(&wolf_client);
    conn_state = CONN_DISCONNECTED;

    pthread_mutex_unlock(&wolf_mtx);
}

void mqtt_loop(void) {
    int rc;
    int read_timeout = 250;

    pthread_mutex_lock(&wolf_mtx);
    rc = MqttClient_WaitMessage(&wolf_client, read_timeout);

    if ((rc == MQTT_CODE_ERROR_TIMEOUT) || (rc == MQTT_CODE_CONTINUE)) {
        // no messages, normal
    } else if (rc != MQTT_CODE_SUCCESS) {
        fprintf(stderr, "Connection lost (rc=%d).\n", rc);
        conn_state = CONN_DISCONNECTED;
    }
    pthread_mutex_unlock(&wolf_mtx);

    // Periodic ping
    int ping_interval = mqtt_conf->keepalive / 2;
    if (ping_interval > MQTT_PING_INTERVAL) ping_interval = MQTT_PING_INTERVAL;
    if (ping_interval < 1) ping_interval = 1;

    if ((conn_state == CONN_CONNECTED) && (time(NULL) - last_ping >= ping_interval)) {
        mqtt_ping();
    }
}

void mqtt_ping(void)
{
    int rc;

    last_ping = time(NULL);

    pthread_mutex_lock(&wolf_mtx);
    fprintf(stderr, "Ping the broker...\n");
    rc = MqttClient_Ping(&wolf_client);
    if (rc != MQTT_CODE_SUCCESS) {
        fprintf(stderr, "Ping failed rc=%d, reconnect...\n", rc);
        conn_state = CONN_DISCONNECTED;
    }
    pthread_mutex_unlock(&wolf_mtx);
}

//-----------------------------------------------------------------------------

void mqtt_init_conf(mqtt_conf_t *conf)
{
    conf->enable=0;
    strcpy(conf->client_id, "yi-cam");

    conf->user=NULL;
    conf->password=NULL;

    strcpy(conf->host, "127.0.0.1");
    strcpy(conf->bind_address, "0.0.0.0");

    conf->port=1883;
    conf->tls=0;
    conf->keepalive=120;
    conf->qos=1;
    conf->retain_birth_will=1;
    conf->retain_motion=1;
    conf->retain_motion_image=1;
    conf->retain_motion_files=1;
    conf->retain_sound_detection=1;

    conf->mqtt_prefix=NULL;
    conf->topic_birth_will=NULL;
    conf->birth_msg=NULL;
    conf->will_msg=NULL;
}

void mqtt_set_conf(mqtt_conf_t *conf)
{
    mqtt_conf=conf;
}

void mqtt_check_connection()
{
    if (conn_state == CONN_CONNECTED) return;

    pthread_mutex_lock(&wolf_mtx);
    fprintf(stderr, "Connection down, reconnecting...\n");
    MqttClient_Disconnect(&wolf_client);
    MqttClient_NetDisconnect(&wolf_client);
    usleep(RECONNECT_DELAY * 1000);
    mqtt_connect();
    pthread_mutex_unlock(&wolf_mtx);
}

int mqtt_connect()
{
    int rc;
    char topic[128];

    fprintf(stderr, "Trying to connect...\n");

    /* Set TLS flag so that MqttClient_NetConnect will attempt TLS when requested */
    if (mqtt_conf->tls) {
        rc = MqttClient_Flags(&wolf_client, 0, MQTT_CLIENT_FLAG_IS_TLS);
    }

    /* Prepare CONNECT packet */
    MqttConnect wolf_connect;
    memset(&wolf_connect, '\0', sizeof(wolf_connect));
    MqttMessage lwt_msg;
    memset(&lwt_msg, '\0', sizeof(lwt_msg));
    wolf_connect.client_id = mqtt_conf->client_id;
    wolf_connect.keep_alive_sec = mqtt_conf->keepalive;
    wolf_connect.clean_session = 1;
    wolf_connect.lwt_msg = &lwt_msg;
    wolf_connect.enable_lwt = 1;

    /* Will */
    lwt_msg.qos        = mqtt_conf->qos;
    lwt_msg.retain     = mqtt_conf->retain_birth_will;
    memset(topic, '\0', sizeof(topic));
    snprintf(topic, sizeof(topic) - 1, "%s/%s", mqtt_conf->mqtt_prefix, mqtt_conf->topic_birth_will);
    lwt_msg.topic_name = (const char *) topic;
    lwt_msg.buffer = (unsigned char *) mqtt_conf->will_msg;
    lwt_msg.total_len  = strlen(mqtt_conf->will_msg);

    if (mqtt_conf->user != NULL && strcmp(mqtt_conf->user, "") != 0) {
        wolf_connect.username = mqtt_conf->user;
        wolf_connect.password = mqtt_conf->password;
    }

    pthread_mutex_lock(&wolf_mtx);
    conn_state = CONN_DISCONNECTED;

    do
    {
        if (!run) {
            pthread_mutex_unlock(&wolf_mtx);
            return -1;
        }

        /* Connect to broker with TLS */
        rc = MqttClient_NetConnect(&wolf_client, mqtt_conf->host, mqtt_conf->port,
            DEFAULT_CON_TIMEOUT, mqtt_conf->tls, mqtt_conf->tls==1? mqtt_tls_cb : NULL);
        if (rc != MQTT_CODE_SUCCESS) {
            fprintf(stderr, "MQTT socket connect error: %s (%d)\n",
                MqttClient_ReturnCodeToString(rc), rc);
            usleep(500*1000);
            continue;
        }

        rc = MqttClient_Connect(&wolf_client, &wolf_connect);
        if (rc != MQTT_CODE_SUCCESS) {
            fprintf(stderr, "MQTT connect error: %s (%d)\n",
                MqttClient_ReturnCodeToString(rc), rc);
            usleep(500*1000);
            continue;
        }
    } while(rc != MQTT_CODE_SUCCESS);

    conn_state = CONN_CONNECTED;
    last_ping = time(NULL);
    send_birth_msg();

    fprintf(stderr, "Connected!\n");
    pthread_mutex_unlock(&wolf_mtx);

    return 0;
}

int mqtt_send_message(mqtt_msg_t *msg, int retain)
{
    int rc;
    MqttPublish wolf_publish;

    if(conn_state != CONN_CONNECTED)
        return -1;

    if ((strlen(msg->topic) >= strlen(EMPTY_TOPIC)) &&
                (strcmp(EMPTY_TOPIC, &msg->topic[strlen(msg->topic) - strlen(EMPTY_TOPIC)]) == 0)) {
        fprintf(stderr, "No message sent: topic is empty\n");
        return -1;
    }

    /* Publish Topic */
    memset(&wolf_publish, '\0', sizeof(wolf_publish));
    wolf_publish.retain = retain;
    wolf_publish.qos = mqtt_conf->qos;
    wolf_publish.duplicate = 0;
    wolf_publish.topic_name = msg->topic;
    wolf_publish.buffer = (unsigned char*) msg->msg;
    wolf_publish.total_len = msg->len;
    pthread_mutex_lock(&wolf_mtx);
    wolf_publish.packet_id = mqtt_get_packetid();
    rc = MqttClient_Publish(&wolf_client, &wolf_publish);

    if ((rc != MQTT_CODE_SUCCESS) && (rc != MQTT_CODE_CONTINUE)) {
        fprintf(stderr, "MQTT Publish error: %s (%d)\n", MqttClient_ReturnCodeToString(rc), rc);
        conn_state = CONN_DISCONNECTED;
    }
    pthread_mutex_unlock(&wolf_mtx);

    return rc;
}

//-----------------------------------------------------------------------------

static int mqtt_tls_cb(MqttClient* client)
{
    client->tls.ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if (client->tls.ctx) {
        wolfSSL_CTX_set_verify(client->tls.ctx, WOLFSSL_VERIFY_PEER, NULL);

        /* Load CA certificate file */
        if (wolfSSL_CTX_load_verify_locations(client->tls.ctx, CA_CERT, NULL) != SSL_SUCCESS) {
            fprintf(stderr, "wolfSSL_CTX_load_verify_locations failed\n");
            return WOLFSSL_FAILURE;
        }
    }

    /* Optional: load client certificate and key for mutual TLS */
    if ((access(CLIENT_CERT, F_OK) == 0) && (access(CLIENT_KEY, F_OK) == 0)) {
        if (wolfSSL_CTX_use_certificate_file(client->tls.ctx, CLIENT_CERT, WOLFSSL_FILETYPE_PEM) != SSL_SUCCESS) {
            fprintf(stderr, "wolfSSL_CTX_use_certificate_file failed\n");
            return WOLFSSL_FAILURE;
        }
        if (wolfSSL_CTX_use_PrivateKey_file(client->tls.ctx, CLIENT_KEY, WOLFSSL_FILETYPE_PEM) != SSL_SUCCESS) {
            fprintf(stderr, "wolfSSL_CTX_use_PrivateKey_file failed\n");
            return WOLFSSL_FAILURE;
        }
    }

    fprintf(stderr, "MQTT TLS setup completed\n");

    return WOLFSSL_SUCCESS;
}

static int mqtt_message_cb(MqttClient *client, MqttMessage *msg,
                           byte msg_new, byte msg_done) {
    (void) client;

    return MQTT_CODE_SUCCESS;
}

static int init_wolf_instance()
{
    int rc;

    /* Prepare network callbacks */
    ctx.sockfd = -1;
    memset(&wolf_net, 0, sizeof(wolf_net));
    wolf_net.context = &ctx;
    wolf_net.connect = net_connect;
    wolf_net.read = net_read;
    wolf_net.write = net_write;
    wolf_net.disconnect = net_disconnect;

    /* Initialize MqttClient structure */
    memset(&wolf_client, 0, sizeof(wolf_client));
    rc = MqttClient_Init(&wolf_client, &wolf_net, mqtt_message_cb,
        tx_buf, sizeof(tx_buf), rx_buf, sizeof(rx_buf),
        DEFAULT_CMD_TIMEOUT);
    if (rc != MQTT_CODE_SUCCESS) {
        fprintf(stderr, "Error in MqttClient_Init: %s (%d)\n",
            MqttClient_ReturnCodeToString(rc), rc);
        return -2;
    }

    return 0;
}
#endif //USE_MOSQUITTO
