#include "mqttv4.h"

/*
 * Just a quick disclaimer.
 * This code is ugly and it is the result of some testing
 * and madness with the MQTT.
 *
 * It will be probably re-written from the ground-up to
 * handle more configurations, messages and callbacks.
 *
 * Warning: No clean exit.
 *
 * Crypto
 */

mqtt_conf_t conf;
mqttv4_conf_t mqttv4_conf;

static void init_mqttv4_config();
static void handle_config(const char *key, const char *value);

files_thread filesThread[2];

int files_delay = 70;        // Wait for xx seconds before search for mp4 files
int files_max_events = 50;   // Number of files reported in the message

int get_thread_index(int isRunning)
{
    int i = -1;
    int ret;
    time_t tmpTimeStart;

    time(&tmpTimeStart);
    for (i = 0; i < 2; i++) {
        if ((isRunning == 1) && (filesThread[i].running == 0)) continue;
        if (filesThread[i].timeStart < tmpTimeStart) {
            tmpTimeStart = filesThread[i].timeStart;
            ret = i;
        }
    }

    return ret;
}

void *send_files_list(void *arg)
{
    char topic[128];
    mqtt_msg_t msg;
    files_thread *ft = (files_thread *) arg;

    sleep(files_delay);

    printf("SENDING FILES LIST\n");

    memset(ft->output, '\0', sizeof(ft->output));
    if (getMp4Files(ft->output, files_max_events, ft->timeStart, ft->timeStop) == 0) {
        msg.msg=ft->output;
        msg.len=strlen(msg.msg);
        msg.topic=topic;

        sprintf(topic, "%s/%s", mqttv4_conf.mqtt_prefix, mqttv4_conf.topic_motion_files);

        mqtt_send_message(&msg, conf.retain_motion_files);
    }
    ft->timeStart = 0;
    ft->timeStop = 0;
    ft->running = 0;

    pthread_exit(NULL);
}

void callback_ai_human_detection_start()
{
    char topic[128];
    mqtt_msg_t msg;

    printf("CALLBACK AI_HUMAN_DETECTION START\n");

    msg.msg=mqttv4_conf.ai_human_detection_start_msg;
    msg.len=strlen(msg.msg);
    msg.topic=topic;

    sprintf(topic, "%s/%s", mqttv4_conf.mqtt_prefix, mqttv4_conf.topic_ai_human_detection);

    mqtt_send_message(&msg, conf.retain_ai_human_detection);
}

void callback_ai_human_detection_stop()
{
    char topic[128];
    mqtt_msg_t msg;

    printf("CALLBACK AI_HUMAN_DETECTION STOP\n");

    msg.msg=mqttv4_conf.ai_human_detection_stop_msg;
    msg.len=strlen(msg.msg);
    msg.topic=topic;

    sprintf(topic, "%s/%s", mqttv4_conf.mqtt_prefix, mqttv4_conf.topic_ai_human_detection);

    mqtt_send_message(&msg, conf.retain_ai_human_detection);
}

void callback_motion_start()
{
    char topic[128];
    char cmd[128];
    char bufferFile[L_tmpnam];
    FILE *fImage;
    long int sz;
    char *bufferImage;
    mqtt_msg_t msg;
    int ti;

    printf("CALLBACK MOTION START\n");

    ti = get_thread_index(0);
    if (ti >= 0 ) {
        time(&filesThread[ti].timeStart);
        filesThread[ti].running = 1;
    }

    // Send start message
    msg.msg=mqttv4_conf.motion_start_msg;
    msg.len=strlen(msg.msg);
    msg.topic=topic;

    sprintf(topic, "%s/%s", mqttv4_conf.mqtt_prefix, mqttv4_conf.topic_motion);

    mqtt_send_message(&msg, conf.retain_motion);

    // Send image
    tmpnam(bufferFile);
    sprintf(cmd, "%s > %s", MQTTV4_SNAPSHOT, bufferFile);
    system(cmd);

    fImage = fopen(bufferFile, "r");
    if (fImage == NULL) {
        printf("Cannot open image file\n");
        remove(bufferFile);
        return;
    }
    fseek(fImage, 0L, SEEK_END);
    sz = ftell(fImage);
    fseek(fImage, 0L, SEEK_SET);

    bufferImage = (char *) malloc(sz * sizeof(char));
    if (bufferImage == NULL) {
        printf("Cannot allocate memory\n");
        fclose(fImage);
        remove(bufferFile);
        return;
    }
    if (fread(bufferImage, 1, sz, fImage) != sz) {
        printf("Cannot read image file\n");
        free(bufferImage);
        fclose(fImage);
        remove(bufferFile);
        return;
    }

    msg.msg=bufferImage;
    msg.len=sz;
    msg.topic=topic;

    sprintf(topic, "%s/%s", mqttv4_conf.mqtt_prefix, mqttv4_conf.topic_motion_image);

    mqtt_send_message(&msg, conf.retain_motion_image);

    // Clean
    free(bufferImage);
    fclose(fImage);
    remove(bufferFile);
}

void callback_motion_stop()
{
    char topic[128];
    mqtt_msg_t msg;
    int ti;
    time_t tmpTimeStop;

    printf("CALLBACK MOTION STOP\n");

    time(&tmpTimeStop);

    msg.msg=mqttv4_conf.motion_stop_msg;
    msg.len=strlen(msg.msg);
    msg.topic=topic;

    sprintf(topic, "%s/%s", mqttv4_conf.mqtt_prefix, mqttv4_conf.topic_motion);

    mqtt_send_message(&msg, conf.retain_motion);

    ti = get_thread_index(1);
    if (ti >= 0 ) {
        filesThread[ti].timeStop = tmpTimeStop;

        if (pthread_create(&filesThread[ti].thread, NULL, send_files_list, (void *) &filesThread[ti])) {
            fprintf(stderr, "An error occured creating thread\n");
        }
        pthread_detach(filesThread[ti].thread);
    }
}

void callback_baby_crying()
{
    char topic[128];
    mqtt_msg_t msg;

    printf("CALLBACK BABY CRYING\n");

    msg.msg=mqttv4_conf.baby_crying_msg;
    msg.len=strlen(msg.msg);
    msg.topic=topic;

    sprintf(topic, "%s/%s", mqttv4_conf.mqtt_prefix, mqttv4_conf.topic_baby_crying);

    mqtt_send_message(&msg, conf.retain_baby_crying);
}

void callback_sound_detection()
{
    char topic[128];
    mqtt_msg_t msg;

    printf("CALLBACK SOUND DETECTION\n");

    msg.msg=mqttv4_conf.sound_detection_msg;
    msg.len=strlen(msg.msg);
    msg.topic=topic;

    sprintf(topic, "%s/%s", mqttv4_conf.mqtt_prefix, mqttv4_conf.topic_sound_detection);

    mqtt_send_message(&msg, conf.retain_sound_detection);
}

int main(int argc, char **argv)
{
    int ret;

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("Starting mqttv4 v%s\n", MQTTV4_VERSION);

    // Init threads struct
    filesThread[0].running = 0;
    filesThread[1].running = 0;
    filesThread[0].timeStart = 0;
    filesThread[1].timeStart = 0;
    filesThread[0].timeStop = 0;
    filesThread[1].timeStop = 0;

    mqtt_init_conf(&conf);
    init_mqttv4_config();
    mqtt_set_conf(&conf);

    ret=init_mqtt();
    if(ret!=0)
        exit(EXIT_FAILURE);

    ret=mqtt_connect();
    if(ret!=0)
        exit(EXIT_FAILURE);

    ret=ipc_init();
    if(ret!=0)
        exit(EXIT_FAILURE);

    ipc_set_callback(IPC_MSG_AI_HUMAN_DETECTION_START, &callback_ai_human_detection_start);
    ipc_set_callback(IPC_MSG_AI_HUMAN_DETECTION_STOP, &callback_ai_human_detection_stop);
    ipc_set_callback(IPC_MSG_MOTION_START, &callback_motion_start);
    ipc_set_callback(IPC_MSG_MOTION_STOP, &callback_motion_stop);
    ipc_set_callback(IPC_MSG_BABY_CRYING, &callback_baby_crying);
    ipc_set_callback(IPC_MSG_SOUND_DETECTION, &callback_sound_detection);

    while(1)
    {
        mqtt_check_connection();
        mqtt_loop();
        usleep(500*1000);
    }

    ipc_stop();
    stop_mqtt();
    stop_config();

    return 0;
}

static void handle_config(const char *key, const char *value)
{
    int nvalue;

    // Ok, the configuration handling is UGLY, unsafe and repetitive.
    // It should be fixed in the future by writing a better config
    // handler or by just using a library.

    // If you think to have a better implementation.. PRs are welcome!

    if(strcmp(key, "MQTT_IP")==0)
    {
        strcpy(conf.host, value);
    }
    else if(strcmp(key, "MQTT_CLIENT_ID")==0)
    {
        strcpy(conf.client_id, value);
    }
    else if(strcmp(key, "MQTT_USER")==0)
    {
        conf.user=malloc((char)strlen(value)+1);
        strcpy(conf.user, value);
    }
    else if(strcmp(key, "MQTT_PASSWORD")==0)
    {
        conf.password=malloc((char)strlen(value)+1);
        strcpy(conf.password, value);
    }
    else if(strcmp(key, "MQTT_PORT")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.port=nvalue;
    }
    else if(strcmp(key, "MQTT_KEEPALIVE")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.keepalive=nvalue;
    }
    else if(strcmp(key, "MQTT_QOS")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.qos=nvalue;
    }
    else if(strcmp(key, "MQTT_RETAIN_BIRTH_WILL")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.retain_birth_will=nvalue;
    }
    else if(strcmp(key, "MQTT_RETAIN_AI_HUMAN_DETECTION")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.retain_ai_human_detection=nvalue;
    }
    else if(strcmp(key, "MQTT_RETAIN_MOTION")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.retain_motion=nvalue;
    }
    else if(strcmp(key, "MQTT_RETAIN_MOTION_IMAGE")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.retain_motion_image=nvalue;
    }
    else if(strcmp(key, "MQTT_RETAIN_MOTION_FILES")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.retain_motion_files=nvalue;
    }
    else if(strcmp(key, "MQTT_RETAIN_BABY_CRYING")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.retain_baby_crying=nvalue;
    }
    else if(strcmp(key, "MQTT_RETAIN_SOUND_DETECTION")==0)
    {
        errno=0;
        nvalue=strtol(value, NULL, 10);
        if(errno==0)
            conf.retain_sound_detection=nvalue;
    }
    else if(strcmp(key, "MQTT_PREFIX")==0)
    {
        conf.mqtt_prefix=malloc((char)strlen(value)+1);
        strcpy(conf.mqtt_prefix, value);
        mqttv4_conf.mqtt_prefix=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.mqtt_prefix, value);
    }
    else if(strcmp(key, "TOPIC_BIRTH_WILL")==0)
    {
        conf.topic_birth_will=malloc((char)strlen(value)+1);
        strcpy(conf.topic_birth_will, value);
        mqttv4_conf.topic_birth_will=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.topic_birth_will, value);
    }
    else if(strcmp(key, "TOPIC_AI_HUMAN_DETECTION")==0)
    {
        mqttv4_conf.topic_ai_human_detection=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.topic_ai_human_detection, value);
    }
    else if(strcmp(key, "TOPIC_MOTION")==0)
    {
        mqttv4_conf.topic_motion=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.topic_motion, value);
    }
    else if(strcmp(key, "TOPIC_MOTION_IMAGE")==0)
    {
        mqttv4_conf.topic_motion_image=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.topic_motion_image, value);
    }
    else if(strcmp(key, "TOPIC_MOTION_FILES")==0)
    {
        mqttv4_conf.topic_motion_files=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.topic_motion_files, value);
    }
    else if(strcmp(key, "TOPIC_BABY_CRYING")==0)
    {
        mqttv4_conf.topic_baby_crying=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.topic_baby_crying, value);
    }
    else if(strcmp(key, "TOPIC_SOUND_DETECTION")==0)
    {
        mqttv4_conf.topic_sound_detection=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.topic_sound_detection, value);
    }
    else if(strcmp(key, "BIRTH_MSG")==0)
    {
        conf.birth_msg=malloc((char)strlen(value)+1);
        strcpy(conf.birth_msg, value);
        mqttv4_conf.birth_msg=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.birth_msg, value);
    }
    else if(strcmp(key, "WILL_MSG")==0)
    {
        conf.will_msg=malloc((char)strlen(value)+1);
        strcpy(conf.will_msg, value);
        mqttv4_conf.will_msg=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.will_msg, value);
    }
    else if(strcmp(key, "AI_HUMAN_DETECTION_START_MSG")==0)
    {
        mqttv4_conf.ai_human_detection_start_msg=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.ai_human_detection_start_msg, value);
    }
    else if(strcmp(key, "AI_HUMAN_DETECTION_STOP_MSG")==0)
    {
        mqttv4_conf.ai_human_detection_stop_msg=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.ai_human_detection_stop_msg, value);
    }
    else if(strcmp(key, "MOTION_START_MSG")==0)
    {
        mqttv4_conf.motion_start_msg=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.motion_start_msg, value);
    }
    else if(strcmp(key, "MOTION_STOP_MSG")==0)
    {
        mqttv4_conf.motion_stop_msg=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.motion_stop_msg, value);
    }
    else if(strcmp(key, "BABY_CRYING_MSG")==0)
    {
        mqttv4_conf.baby_crying_msg=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.baby_crying_msg, value);
    }
    else if(strcmp(key, "SOUND_DETECTION_MSG")==0)
    {
        mqttv4_conf.sound_detection_msg=malloc((char)strlen(value)+1);
        strcpy(mqttv4_conf.sound_detection_msg, value);
    }
    else
    {
        printf("key: %s | value: %s\n", key, value);
        fprintf(stderr, "Unrecognized config.\n");
    }
}

static void init_mqttv4_config()
{
    // Setting conf vars to NULL
    mqttv4_conf.mqtt_prefix=NULL;
    mqttv4_conf.topic_birth_will=NULL;
    mqttv4_conf.topic_motion=NULL;
    mqttv4_conf.topic_motion_image=NULL;
    mqttv4_conf.topic_motion_files=NULL;
    mqttv4_conf.topic_baby_crying=NULL;
    mqttv4_conf.topic_sound_detection=NULL;
    mqttv4_conf.topic_ai_human_detection=NULL;
    mqttv4_conf.birth_msg=NULL;
    mqttv4_conf.will_msg=NULL;
    mqttv4_conf.ai_human_detection_start_msg=NULL;
    mqttv4_conf.ai_human_detection_stop_msg=NULL;
    mqttv4_conf.motion_start_msg=NULL;
    mqttv4_conf.motion_stop_msg=NULL;
    mqttv4_conf.baby_crying_msg=NULL;
    mqttv4_conf.sound_detection_msg=NULL;

    if(init_config(MQTTV4_CONF_FILE)!=0)
    {
        printf("Cannot open config file. Skipping.\n");
        return;
    }

    config_set_handler(&handle_config);
    config_parse();

    // Setting default for all char* vars
    if(conf.mqtt_prefix == NULL)
    {
        conf.mqtt_prefix=malloc((char)strlen("yicam")+1);
        strcpy(conf.mqtt_prefix, "yicam");
    }
    if(mqttv4_conf.mqtt_prefix == NULL)
    {
        mqttv4_conf.mqtt_prefix=malloc((char)strlen("yicam")+1);
        strcpy(mqttv4_conf.mqtt_prefix, "yicam");
    }
    if(conf.topic_birth_will == NULL)
    {
        conf.topic_birth_will=malloc((char)strlen("status")+1);
        strcpy(conf.topic_birth_will, "status");
    }
    if(mqttv4_conf.topic_birth_will == NULL)
    {
        mqttv4_conf.topic_birth_will=malloc((char)strlen("status")+1);
        strcpy(mqttv4_conf.topic_birth_will, "status");
    }
    if(mqttv4_conf.topic_ai_human_detection == NULL)
    {
        mqttv4_conf.topic_ai_human_detection=malloc((char)strlen("ai_human_detection")+1);
        strcpy(mqttv4_conf.topic_ai_human_detection, "ai_human_detection");
    }
    if(mqttv4_conf.topic_motion == NULL)
    {
        mqttv4_conf.topic_motion=malloc((char)strlen("motion_detection")+1);
        strcpy(mqttv4_conf.topic_motion, "motion_detection");
    }
    if(mqttv4_conf.topic_motion_image == NULL)
    {
        mqttv4_conf.topic_motion_image=malloc((char)strlen("motion_detection_image")+1);
        strcpy(mqttv4_conf.topic_motion_image, "motion_detection_image");
    }
    if(mqttv4_conf.topic_motion_files == NULL)
    {
        mqttv4_conf.topic_motion_files=malloc((char)strlen("motion_files")+1);
        strcpy(mqttv4_conf.topic_motion_files, "motion_files");
    }
    if(mqttv4_conf.topic_baby_crying == NULL)
    {
        mqttv4_conf.topic_baby_crying=malloc((char)strlen("baby_crying")+1);
        strcpy(mqttv4_conf.topic_baby_crying, "baby_crying");
    }
    if(mqttv4_conf.topic_sound_detection == NULL)
    {
        mqttv4_conf.topic_sound_detection=malloc((char)strlen("sound_detection")+1);
        strcpy(mqttv4_conf.topic_sound_detection, "sound_detection");
    }
    if(conf.birth_msg == NULL)
    {
        conf.birth_msg=malloc((char)strlen("online")+1);
        strcpy(conf.birth_msg, "online");
    }
    if(mqttv4_conf.birth_msg == NULL)
    {
        mqttv4_conf.birth_msg=malloc((char)strlen("online")+1);
        strcpy(mqttv4_conf.birth_msg, "online");
    }
    if(conf.will_msg == NULL)
    {
        conf.will_msg=malloc((char)strlen("offline")+1);
        strcpy(conf.will_msg, "offline");
    }
    if(mqttv4_conf.will_msg == NULL)
    {
        mqttv4_conf.will_msg=malloc((char)strlen("offline")+1);
        strcpy(mqttv4_conf.will_msg, "offline");
    }
    if(mqttv4_conf.ai_human_detection_start_msg == NULL)
    {
        mqttv4_conf.ai_human_detection_start_msg=malloc((char)strlen("human_start")+1);
        strcpy(mqttv4_conf.ai_human_detection_start_msg, "human_start");
    }
    if(mqttv4_conf.ai_human_detection_stop_msg == NULL)
    {
        mqttv4_conf.ai_human_detection_stop_msg=malloc((char)strlen("human_stop")+1);
        strcpy(mqttv4_conf.ai_human_detection_stop_msg, "human_stop");
    }
    if(mqttv4_conf.motion_start_msg == NULL)
    {
        mqttv4_conf.motion_start_msg=malloc((char)strlen("motion_start")+1);
        strcpy(mqttv4_conf.motion_start_msg, "motion_start");
    }
    if(mqttv4_conf.motion_stop_msg == NULL)
    {
        mqttv4_conf.motion_stop_msg=malloc((char)strlen("motion_stop")+1);
        strcpy(mqttv4_conf.motion_stop_msg, "motion_stop");
    }
    if(mqttv4_conf.baby_crying_msg == NULL)
    {
        mqttv4_conf.baby_crying_msg=malloc((char)strlen("crying")+1);
        strcpy(mqttv4_conf.baby_crying_msg, "crying");
    }
    if(mqttv4_conf.sound_detection_msg == NULL)
    {
        mqttv4_conf.sound_detection_msg=malloc((char)strlen("sound")+1);
        strcpy(mqttv4_conf.sound_detection_msg, "sound");
    }
}
