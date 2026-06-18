/* 海凌科MQTT私有云接入  */
/******************************************************************************

                        Copyright (C), 

 ******************************************************************************
  Filename   : hi_mqtt.h
  Version    : v1.0
  Author     : 
  Creation   : 
  Description: 
 *****************************************************************************/


#ifndef __HI_MQTT_H
#define __HI_MQTT_H

// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <unistd.h>
#include "client/MQTTClient.h"

#ifdef HLK_PRODUCT_WR10
#include "os/hi_os_thread.h"
#endif

#include "hi_sock.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

/*****************************************************************************
*                                DEFINE                                      *
*****************************************************************************/

#define SYSUPGRADE_BIN_PATH_TMP "/tmp/tmp_app.bin"
#define SYSUPGRADE_BIN_PATH "/tmp/app.bin"
#define SYSUPGRADE_MSGID_PATH "/mnt/data/ota_ver.bin"

//配置文件
#define MQTT_CONFIG "hlk_mqtt"
#define MQTT_CONNECT_SECTION "mqtt_connect"

/* 主题 */
#define TOPIC_LEN 128

/* 海凌科mqtt平台连接参数 */
// 连网方式:
// NET_LORA(表示LoRa)           NET CELLULAR(表示2G/3G/4G/5G蜂窝网)
// NET WIFI(表示Wi-Fi)          NET ZIGBEE(表示ZigBee)
// NET_ETHERNET(表示以太网)     NET_OTHER(表示其他网络类型)
#define NET_TYPE            "NET_ETHERNET"
#ifdef HLK_PRODUCT_WR10
#define MODULE_TYPE          "WR10"
#elif HLK_PRODUCT_RM65
#define MODULE_TYPE          "RM65"
#elif HLK_PRODUCT_RM68
#define MODULE_TYPE          "RM68"
#elif HLK_PRODUCT_RM60
#define MODULE_TYPE          "RM60"
#elif HLK_PRODUCT_RM50
#define MODULE_TYPE          "RM50"
#elif HLK_PRODUCT_7628
#define MODULE_TYPE          "MT7628"   //用来测试 改成RM65
#endif

#if defined(HLK_PRODUCT_WR10) || defined(HLK_PRODUCT_7628)
#define WAN_INTERFACE          "br-lan"
#elif defined(HLK_PRODUCT_RM65)
#define WAN_INTERFACE          "br-lan"
#else
#define WAN_INTERFACE          "br-lan"
#endif

/* 解析云端数据 */
#define AppToWifi "AppToWifi"
#define WifiToApp "WifiToApp"
#define WifiToServer "WifiToServer"
#define ServerToWifi "ServerToWifi"
#define InputData "InputData"

#define MQTT_CONNECT_STATUS_CONNECTING 1
#define MQTT_CONNECT_STATUS_CONNECTED 2
#define MQTT_CONNECT_STATUS_DISCONNECTED 3

#define IS_START 1

// sys/{ProductKey}/{DeviceCode}/thing/property/post
/* 数据上报及回复 */
//#define TOPIC_POST "sys/%s/%s/thing/property/post"
//#define TOPIC_POST_REPLY "sys/%s/%s/thing/property/post_reply"

/* 心跳包上报及回复 */
//#define TOPIC_PING_POST "sys/%s/%s/thing/event/ping/post"
//#define TOPIC_POST_PING_REPLY "sys/%s/%s/thing/event/ping/post_reply"

#define N(A) (sizeof(A)/sizeof(A[0]))

/*****************************************************************************
 *                                TYPEDEF                                    *
 *****************************************************************************/
typedef struct mqtt_app_heartbeat{
    unsigned long total_memory;
    unsigned long free_memory;
    unsigned long disk_size;
    unsigned long free_disk_size;
    float cpu_rate;
    double temperature;
    double battery;
    char local_ip[16];
    unsigned int uptime;
    unsigned long utc_time;
    unsigned int delay;
    char module[32];
    char version[128];
    char *imei;
    char *iccid;
    char *imsi;
    char imei_data[32];
    char iccid_data[32];
    char imsi_data[32];
}MQTT_APP_HEATBEAT_S;

typedef struct mqtt_user_certification{
    char projectID[64];
    char productKey[64];
    char productSecret[64];
    char deviceName[64];
    char deviceSecret[64];
/*mqtt other*/
    char clientID[64];
    char username[64];
    char password[64];
/*mqtt other*/
    int keepalive;
    int cleanSession;
}MQTT_USER_CERT_S;

/* 线程间通信操作对像 */
typedef struct
{
    int response_status;/*订阅、取消订阅、发布是否成功的状态*/
    int s_recv_flag;
    int c_recv_flag;
    int reponse_time_limmit;
    TOPIC_PACKET_S *ptopic_packet;
    pthread_mutex_t mutex;
    int connect_status; //云端连接状态  0:未连接 1:连接中 2:连接成功 3:连接失败
    int flowtype;               //流量类型，0表示每月清空， 1表示每年清空，云端仅当上报心跳同时携带IMEI和ICCID且都有值时返回
    double flowsize;            //当前有效总套餐流量大小， 这个是包括加油包？
    double current_month_flow;  //当月流量快照  这个当前月份已经使用的流量 如果FlowType == 0 就通过这个字段判断是否超出了当前套餐的流量
    double current_year_flow;   //当前流量快照  这个当前年份已经使用的流量 如果FlowType == 1 就通过这个字段判断是否超出了当前套餐的流量
    int current_flow_year;     //当前流量快照所属年份
    int current_month;         //当前流量快照所属月份
    int isblocked;             //内置卡是否被阻断 0:未阻断 1:阻断
    int isnextmonth;           //是否是次月 0:否 1:是
}SHARED_DATA_S;

/* 海凌科iot云操作对象 */
typedef struct
{
    int connnect_type;
    Network *network;   
    MQTTClient *client; 
    MQTT_USER_CERT_S *pmqtt_user_cert;
    int (*func)(SHARED_DATA_S *);
} HLK_IOT_S;

typedef struct mqtt_connect_cret{
    char id[128];
    char user_name[128];
    char password[128];
}MQTT_CONNECT_CRET_S;

typedef struct mqtt_pub_sub_pattern{
    int pattern;
    char *format;
    char topic[TOPIC_LEN];
}MQTT_PUB_SUB_PATTERN_S;

typedef struct mqtt_pub_data{
    int QOS;
    char timed;
    char topic[TOPIC_LEN];
    char *data;
}MQTT_PUB_DATA_S;

typedef struct m_http_post_headers_s{
    char acLng[32];
    char acId[32];
    char acNonce[32];
    char acTimeStamp[32];
    char acAccessId[32];
    char acSignature[64];
}M_HTTP_POST_HEADERS_S;

typedef struct m_http_post_body_s{
    char acCode[32];
    int iStart;
    int iSize;
    char acBody[128];
}M_HTTP_POST_BODY_S;

typedef enum
{
    MQTT_CONNECT_HLK,
    MQTT_CONNECT_OTHER,
}MQTT_CONNECT_TYPE_E;

typedef enum
{
    MQTT_TOPIC_TYPE_START = 0,
    TOPIC_PING_POST,
    TOPIC_POST_PING_REPLY,
    TOPIC_POST,
    TOPIC_POST_REPLY,
    TOPIC_GET,
    TOPIC_GET_REPLY,
    TOPIC_UPGRADE,
    TOPIC_UPGRADE_REPLY,
    TOPIC_RESET,
    TOPIC_RESET_REPLY,
    TOPIC_APP,
    DATA_POINTS_UP,
    DATA_POINTS_DOWN,
    FLOW_UPDATE,
    FLOW_UPDATE_CONFIRM,
    MQTT_TOPIC_TYPE_END
}MQTT_TOPIC_TYPE_E;

typedef enum
{
    HLK_OTA_NO_ERR = 0,
    HLK_OTA_VER_ERR,
    HLK_OTA_FLASH_CHECK_ERR,
    HLK_OTA_URL_ERR
}MQTT_SYSUPGRADE_UP_E;

typedef enum {
    CHECKOTA_STATUS_NO_UPGRADE_OR_UPGRADED = 2,
    CHECKOTA_STATUS_UPGRADING = 1,
    CHECKOTA_STATUS_UPGRADE_CANCEL = 3,
    CHECKOTA_STATUS_UPGRADE_FAILED = 4,
} CHECKOTA_STATUS_E;


#pragma pack(push, 1) 
typedef struct
{
    uint32_t msgid;     // msgid
    int progress;       // 升级流程 0未开始 1已完成 2升级中
    char version[64];   // 版本号
} VersionInfo;
#pragma pack(pop)

/*****************************************************************************
*                                FUNCTION                                    *
*****************************************************************************/
extern MQTT_USER_CERT_S mqtt_user_cert; // MQTT用户认证信息结构体

extern int hlk_mqtt_ping(int is_start);
extern void mqtt_topic_type_init();
extern void mqtt_subscribe_parse();
extern int hlk_mqtt_main();
extern int hlk_mqtt_publish(char *topic, int qos, void *data, int len);
extern int hlk_ota_read_version(const char *filename, VersionInfo *out);

// extern void messageArrived(MessageData *data);

// extern void get_mqtt_user_certification_o();//uci获取other mqtt的认证信息
extern int get_mqtt_user_certification_h();//获取hlk mqtt的认证信息

extern MQTT_USER_CERT_S mqtt_user_cert;
extern MQTT_CONNECT_CRET_S mqtt_connect_cret;
extern HLK_IOT_S hlk_iot;

extern SHARED_DATA_S sharedData;
extern MQTT_PUB_SUB_PATTERN_S mqtt_topic_type_table[];

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __HI_MQTT_H*/
