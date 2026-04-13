// MQTT私有云连接功能实现文件
// 本文件实现了设备与私有云平台的MQTT连接、认证、消息收发等核心功能
// 支持设备注册、心跳上报、OTA升级、设备重置等功能

// 系统头文件包含
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/time.h>
#include <sys/select.h>
#include "hlk_log.h"

// ============================================================================
// Musl 1.2+ compatibility: Forward declarations for __*time64 symbols
// ============================================================================
// OpenWrt's musl 1.2.x uses 64-bit time_t but does not export symbols with
// the __time64 suffix (like glibc does). When the linker tries to resolve
// these missing symbols at runtime, we provide forwarding functions that
// call the standard musl functions.
//
// These are defined as actual functions (not just aliases) so they are
// guaranteed to be linked into the binary even with symbol stripping.

#ifdef __MUSL__

// Forward the __*time64 calls to standard musl functions
// Use default visibility to ensure these functions are exported for linking
__attribute__((visibility("default")))
int __nanosleep_time64(const struct timespec *req, struct timespec *rem) {
    return nanosleep(req, rem);
}

__attribute__((visibility("default")))
int __select_time64(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, struct timeval *timeout) {
    return select(nfds, readfds, writefds, exceptfds, timeout);
}

__attribute__((visibility("default")))
time_t __time64(time_t *tloc) {
    return time(tloc);
}

__attribute__((visibility("default")))
int __gettimeofday_time64(struct timeval *tv, void *tz) {
    return gettimeofday(tv, (struct timezone *)tz);
}

__attribute__((visibility("default")))
struct tm *__localtime64(const time_t *timep) {
    return localtime(timep);
}

__attribute__((visibility("default")))
time_t __mktime64(struct tm *tm) {
    return mktime(tm);
}

__attribute__((visibility("default")))
int __settimeofday_time64(const struct timeval *tv, const struct timezone *tz) {
    return settimeofday(tv, tz);
}

__attribute__((visibility("default")))
double __difftime64(time_t time1, time_t time0) {
    return difftime(time1, time0);
}

#include <sys/stat.h> 
// ========================================================
// 【新增】补充缺失的 stat/fstat Time64 兼容接口
// ========================================================
__attribute__((visibility("default")))
int __fstat_time64(int fd, struct stat *st) {
    return fstat(fd, st);
}

__attribute__((visibility("default")))
int __stat_time64(const char *path, struct stat *st) {
    return stat(path, st);
}

__attribute__((visibility("default")))
int __lstat_time64(const char *path, struct stat *st) {
    return lstat(path, st);
}

#endif
// ============================================================================

#ifdef ENABLE_CURL
// 避免curl包含time.h导致的冲突
#define CURL_DISABLE_TYPECHECK 1
#include "curl/curl.h"
#endif

// 海凌科操作系统相关头文件
#ifdef HLK_PRODUCT_WR10
#include "os/hi_os_thread.h"     // 线程操作接口
#include "os/hi_os_fileio.h"     // 文件IO操作接口
#elif defined(HLK_PRODUCT_RM65) || defined(HLK_PRODUCT_RM50)
#include "openwrt/openwrt.h"
#elif defined(HLK_PRODUCT_7628)
#include "linux/linux.h"
#endif

// 项目自定义头文件
#ifdef HLK_PRODUCT_WR10
#include "hi_cfm_api.h"          // 配置管理API
#endif

#include "../include/md5.h"                 // MD5加密算法
#include "cJSON.h"               // JSON解析库
#include "yyjson.h"              // 高性能JSON解析库
#include "hi_mqtt.h"             // MQTT相关定义
#include "hi_link.h"             // 设备连接相关
#include "hi_link_ipc.h"         // IPC相关定义
#include "app_api.h"             // Zig实现的函数声明
#include "app_api.h"             // 应用程序API
#ifdef HLK_PRODUCT_WR10
#include "igdCmApi.h"            // 网关管理API
#endif

#include "device_credentials.h"

// 在文件顶部添加管道相关的头文件和定义
#include <sys/stat.h>
#include <fcntl.h>

// 定义SSE数据推送管道路径
#define SSE_DATA_PIPE_PATH "/tmp/sse_data_pipe"

// 添加数据推送函数声明
static int push_data_to_sse(const char *channel, const char *data);

// 在文件顶部添加需要的头文件
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

// 添加libcurl和cJSON的头文件
#include <curl/curl.h>
#include <cJSON.h>

// 添加OpenSSL头文件用于SHA1
#include <openssl/sha.h>

// 定义子进程通信结构体
typedef struct {
    int result_code;        // 下载结果代码
    int download_progress;  // 下载进度
    char error_msg[256];    // 错误信息
} ota_download_result_t;

// 定义内存结构体用于存储HTTP响应数据
struct MemoryStruct {
    char *memory;
    size_t size;
};

// 定义下载超时时间（秒）
#define OTA_DOWNLOAD_TIMEOUT 1800  // 30分钟

/*****************************************************************************
 * 测试使用的私有云设备信息（用于开发调试阶段）
 * deviceName: TOjqhu0Ahuh        - 设备名称标识符
 * projectKey: dmT7FJRR0000000X   - 项目密钥
 * productKey: bmIAARC000B        - 产品密钥  
 * productSecret: dfUdMDmyO9uZ19AZ - 产品密码
 * deviceSecret: DTCl2pQZ0OL3STWz  - 设备密码
 *****************************************************************************/

// 缓冲区大小定义
#define BUFFER_SIZE 1024

// MQTT和WebAPI地址相关宏定义
#define MQTT_URL_COUNT 2
#define MQTT_URL_LEN 32
#define MQTT_URL "cloud.hlktech.com"

// 请求地址相关宏定义
#define REQUEST_ADDRESS "http://fl.hlktech.com/api/v1/Urls/GetHost"
#define REQUEST_ADDRESS_TOKEN "93GFQBbLjAHrR85TdnrlQI2nKRFk5d7r"
#define REQUEST_ACCESSID "H0002"

// API路径宏定义
#define POST_INIF_API "/api/iot/device/init"
#define POST_HISTORY_API "/api/iot/device/history"
#define POST_OTA_INFO "/api/iot/device/ota"

#define PRODUCT_KEY_MAXLEN          (20 + 1)
#define PRODUCT_ID_MAXLEN          (20 + 1)
#define DEVICE_NAME_MAXLEN          (32 + 1)
#define DEVICE_ID_MAXLEN            (64 + 1)
#define DEVICE_SECRET_MAXLEN        (64 + 1)
#define PRODUCT_SECRET_MAXLEN       (64 + 1)

// 设备信息结构体定义
typedef struct
{
    unsigned int magic;
    char productKey[PRODUCT_KEY_MAXLEN + 1];
    char productSecret[DEVICE_SECRET_MAXLEN + 1];
    char deviceName[DEVICE_NAME_MAXLEN + 1];
    char deviceSecret[PRODUCT_KEY_MAXLEN + 1];
    char projectKey[PRODUCT_KEY_MAXLEN];
} ALINKDEV_t;

// 全局变量定义
static int g_mqtt_flag = 0;           // MQTT连接状态标志位
MQTT_USER_CERT_S mqtt_user_cert = {0}; // MQTT用户认证信息结构体

// MQTT和WebAPI地址全局变量
char mqttUrl[50];
char webApiUrl[50];
char postInfoUrl[200];
char postHistoryUrl[200];
char postOtaInfoUrl[200];

// 海凌科IoT连接结构体初始化
HLK_IOT_S hlk_iot = {
    .client = NULL,                    // MQTT客户端指针
    .pmqtt_user_cert = NULL,          // 用户认证信息指针
    .connnect_type = MQTT_CONNECT_HLK  // 连接类型：海凌科MQTT平台
};

// MQTT主题类型映射表 - 定义各种消息类型对应的主题格式
MQTT_PUB_SUB_PATTERN_S mqtt_topic_type_table[] = {
    {MQTT_TOPIC_TYPE_START,     NULL,                                           "0"}, // 起始标记
    {TOPIC_PING_POST,           "sys/%s/%s/thing/event/ping/post",              "0"}, // 心跳发送主题
    {TOPIC_POST_PING_REPLY,     "sys/%s/%s/thing/event/ping/post_reply",        "0"}, // 心跳回复主题
    {TOPIC_POST,                "sys/%s/%s/thing/property/post",                "0"}, // 属性上报主题
    {TOPIC_POST_REPLY,          "sys/%s/%s/thing/property/post_reply",          "0"}, // 属性回复主题
    {TOPIC_GET,                 "sys/%s/%s/thing/service/post",                 "0"}, // 服务调用主题
    {TOPIC_GET_REPLY,           "sys/%s/%s/thing/service/post_reply",           "0"}, // 服务回复主题
    {TOPIC_UPGRADE,             "sys/%s/%s/thing/ota/get",                      "0"}, // OTA升级主题
    {TOPIC_UPGRADE_REPLY,       "sys/%s/%s/thing/ota/get_reply",                "0"}, // OTA回复主题
    {TOPIC_RESET,               "sys/%s/%s/thing/event/reset",                  "0"}, // 设备重置主题
    {TOPIC_RESET_REPLY,         "sys/%s/%s/thing/event/reset_reply",            "0"}, // 重置回复主题
    {TOPIC_APP,                 "sys/%s/%s/thing/property/set",                 "0"}, // APP控制主题
    {DATA_POINTS_UP,            "sys/%s/%s/thing/property/DataPointsUp",        "0"}, //采集数据上报主题
    {DATA_POINTS_DOWN,          "sys/%s/%s/thing/property/DataPointsDown",    "0"}, //采集数据上报主题
    {MQTT_TOPIC_TYPE_END,       NULL,                                           "0"}  // 结束标记
};

// MQTT连接认证信息结构体
MQTT_CONNECT_CRET_S mqtt_connect_cret = {0};


/******************************************************************************
 * 函数名    : replace_https_with_http
 * 功能描述  : 将URL中的https替换为http
 * 输入参数  : url - 要处理的URL字符串
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 用于将HTTPS URL转换为HTTP URL
 ******************************************************************************/
void replace_https_with_http(char *url)
{
    if (url == NULL) return;

    const char *https_prefix = "https://";
    const char *http_prefix = "http://";

    if (strncmp(url, https_prefix, strlen(https_prefix)) == 0) {
        // 将https://替换为http://
        memmove(url + strlen(http_prefix), url + strlen(https_prefix),
                strlen(url) - strlen(https_prefix) + 1);
        memcpy(url, http_prefix, strlen(http_prefix));
    }
}

/******************************************************************************
 * 函数名    : get_device_credentials
 * 功能描述  : 获取设备认证信息
 * 输入参数  : dev - 指向设备信息结构体的指针
 * 输出参数  : 无
 * 返回值    : 0-成功 -1-失败
 * 说明      : 从配置管理模块获取设备五元组信息
 ******************************************************************************/
int get_device_credentials(ALINKDEV_t *dev)
{
    if (dev == NULL) return -1;

    // 从配置管理模块获取设备五元组信息
    int ret = cfmGetLicense(dev->deviceName, dev->projectKey, dev->productKey,
                           dev->productSecret, dev->deviceSecret, 64);

    if(ret != 0){
        HLK_LOG_ERR("cfmGetLicense failed!\n");
        return -1;
    }

    return 0;
}

/********* OTA升级相关全局变量 *********/
uint32_t g_ota_msgid = 0;  // OTA升级消息ID
uint32_t g_ota_fid = 0;    // OTA升级文件ID  
uint32_t g_ota_size = 0;   // OTA升级文件大小
size_t g_wirtten;          // 已写入字节数
size_t g_curl_schedule = 0;

/******************************************************************************
 * 函数名    : fota_Upgrade_Writing
 * 功能描述  : OTA升级 - 下载完成后将固件写入设备存储
 * 输入参数  : void
 * 输出参数  : N/A
 * 返回值    : 0-成功 其他-失败
 * 说明      : 调用系统固件升级API完成最终的固件烧录
 ******************************************************************************/
int fota_Upgrade_Writing(void)
{
    unsigned int ret = 0;

    #if defined(HLK_PRODUCT_WR10)
    IgdSysmngFirmwareAttrTab filestru;  // 固件属性结构体
    
    // 设置固件文件路径和属性
    memcpy((char *)filestru.aucFirmwareName, SYSUPGRADE_BIN_PATH, sizeof(filestru.aucFirmwareName));
    filestru.ulOffset = 0;                    // 偏移量为0，从头开始
    filestru.ulFirmwareSize = g_ota_size;     // 固件文件大小
    filestru.ucReboot = 0;                    // 升级后是否重启
    
    HLK_LOG_INFO("aucFirmwareName=%s,ulFirmwareSize=%d\r\n", filestru.aucFirmwareName, filestru.ulFirmwareSize);
    
    // 调用系统固件升级API
    ret = igdCmFirmwareUpgradeApi((unsigned char *)&filestru, sizeof(filestru));
    
    #elif defined(HLK_PRODUCT_RM65)
    //RM65固件升级
    //固件组成：固件头 MD5 固件 32字节  更新内容 1024-32字节，固件 
    #elif defined(HLK_PRODUCT_RM50)
    openwrt_upgrade_firmware();
    #elif defined(HLK_PRODUCT_7628)
    //7628固件升级
    //固件组成：固件头 MD5 固件 32字节  更新内容 1024-32字节，固件 
    //1. 禁止页面升级
    mt7628_upgrade_firmware();
    #endif

    return ret;
}

/******************************************************************************
 * 函数名    : get_mqtt_user_certification_h
 * 功能描述  : 获取海凌科MQTT平台的设备认证信息（五元组）
 * 输入参数  : 无
 * 输出参数  : N/A
 * 返回值    : 0-成功 -1-失败
 * 说明      : 从配置文件中读取设备的五元组信息，用于MQTT连接认证
 ******************************************************************************/
int get_mqtt_user_certification_h()
{
    // 定义临时存储五元组信息的变量
    char deviceName[64] = {0};      // 设备名称
    char projectKey[64] = {0};      // 项目密钥
    char productKey[64] = {0};      // 产品密钥
    char productSecret[64] = {0};   // 产品密码
    char deviceSecret[64] = {0};    // 设备密码
    
    // 从配置管理模块获取设备五元组信息
    int ret = cfmGetLicense(deviceName, projectKey, productKey, productSecret, deviceSecret, 64);
    
    if(ret != 0){
        // 获取五元组信息失败
        HLK_LOG_ERR("cfmGetLicense failed!\r\n");
        return -1;
    }

    // 将获取到的五元组信息复制到全局认证结构体中
    strcpy(mqtt_user_cert.projectID, deviceName);
    strcpy(mqtt_user_cert.productKey, productKey);
    strcpy(mqtt_user_cert.productSecret, productSecret);
    strcpy(mqtt_user_cert.deviceName, deviceName);
    strcpy(mqtt_user_cert.deviceSecret, deviceSecret);

    // 打印认证信息用于调试
    HLK_LOG_INFO("projectID: %s\n", mqtt_user_cert.projectID);    
    HLK_LOG_INFO("productKey: %s\n", mqtt_user_cert.productKey);    
    HLK_LOG_INFO("productSecret: %s\n", mqtt_user_cert.productSecret);    
    HLK_LOG_INFO("deviceName: %s\n", mqtt_user_cert.deviceName);    
    HLK_LOG_INFO("deviceSecret: %s\n", mqtt_user_cert.deviceSecret);   

    return 0;
}

/******************************************************************************
 * 函数名    : hlk_topic_make
 * 功能描述  : MQTT主题字符串拼接生成
 * 输入参数  : format - 主题格式字符串（包含%s占位符）
 * 输出参数  : topic - 拼接完成的主题字符串
 * 返回值    : 无
 * 说明      : 使用productKey和deviceName替换格式字符串中的占位符
 ******************************************************************************/
void hlk_topic_make(char *topic, char *format)
{
    char *_topic = NULL;

    // 动态分配内存存储临时主题字符串
    _topic = (char *)malloc(sizeof(char) * TOPIC_LEN);
    if (_topic != NULL)
    {
        // 使用sprintf将产品密钥和设备名称填入主题格式中
        sprintf(_topic, format, mqtt_user_cert.productKey, mqtt_user_cert.deviceName);
        
        // 将结果复制到输出参数中
        strcpy(topic, _topic);
        free(_topic);  // 释放临时内存
    }
   
    return;
}

/******************************************************************************
 * 函数名    : hlk_mqtt_publish
 * 功能描述  : MQTT消息发布函数
 * 输入参数  : topic - 发布的主题
 *            qos   - 服务质量等级（0,1,2）
 *            data  - 消息数据指针
 *            len   - 消息数据长度
 * 返回值    : MQTT发布结果
 * 说明      : 封装MQTT消息发布操作，设置QoS、重复标志等参数
 ******************************************************************************/
int hlk_mqtt_publish(char *topic, int qos, void *data, int len)
{
    MQTTMessage pubmsg;        // MQTT消息结构体
    pubmsg.qos = qos;          // 设置服务质量等级
    pubmsg.retained = 0;       // 不保留消息
    pubmsg.payload = data;     // 消息载荷数据
    pubmsg.payloadlen = len;   // 载荷数据长度
    pubmsg.dup = 0;           // 非重复消息

    PRF("#### Topic: %s Data: %s\n", topic, (char*)data);

    // 调用MQTT库发布消息
    return MQTTPublish(hlk_iot.client, topic, &pubmsg);
}

/******************************************************************************
 * 函数名    : hlk_mqtt_heartbeat_get
 * 功能描述  : 收集设备系统信息用于MQTT心跳包
 * 输入参数  : 无
 * 输出参数  : mqtt_app_heartbea - 填充完成的心跳信息结构体
 * 返回值    : 无
 * 说明      : 收集设备的各种系统状态信息，包括内存、磁盘、CPU、温度等
 *            这些信息将被打包成JSON格式的心跳包发送给云端
 ******************************************************************************/
void hlk_mqtt_heartbeat_get(MQTT_APP_HEATBEAT_S *mqtt_app_heartbea)
{
    // 获取内存使用情况（总内存和空闲内存）
    get_mem_info(&mqtt_app_heartbea->total_memory, &mqtt_app_heartbea->free_memory);
    
    // 获取磁盘使用情况（总容量和剩余空间）
    get_disk_info(&mqtt_app_heartbea->disk_size, &mqtt_app_heartbea->free_disk_size);
    
    // 获取CPU使用率
    get_cpu_info(&mqtt_app_heartbea->cpu_rate);
    
    // 获取设备温度信息
    get_temp_info(&mqtt_app_heartbea->temperature);
    
    // 获取电池电量信息（如果设备支持）
    get_battery_info(&mqtt_app_heartbea->battery);
    
    // 获取设备本地IP地址
    get_local_ip(mqtt_app_heartbea->local_ip);
    
    // 获取系统运行时间（开机时长）
    get_uptime_info(&mqtt_app_heartbea->uptime);
    
    // 获取UTC时间戳
    get_utc_time_info(&mqtt_app_heartbea->utc_time);
    
    // 获取网络延迟信息
    get_delay_info(&mqtt_app_heartbea->delay);
    
    // 获取设备模块类型信息
    get_module_info(mqtt_app_heartbea->module);
    
    // 获取设备固件版本信息
    get_version_info(mqtt_app_heartbea->version);
    return;
}

/******************************************************************************
 * 函数名    : hlk_mqtt_ping
 * 功能描述  : 发送MQTT心跳包到云端服务器
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 0-成功 -1-失败
 * 说明      : 定期向云端发送设备状态信息，保持连接活跃性
 *            心跳包包含设备的详细系统信息，供云端监控设备状态
 ******************************************************************************/
int hlk_mqtt_ping(void)
{
    int ret;
    MQTT_APP_HEATBEAT_S mqtt_app_heartbeat = {0};
    
    // 清空心跳数据结构体
    memset(&mqtt_app_heartbeat, 0, sizeof(MQTT_APP_HEATBEAT_S));
    
    // 收集当前设备的系统状态信息
    hlk_mqtt_heartbeat_get(&mqtt_app_heartbeat);

    // 创建JSON对象用于构建心跳包
    cJSON *root = NULL;
    root = cJSON_CreateObject();
    if(root == NULL){
        HLK_LOG_ERR("Failed to allocate memory for JSON object.");
        return -1;
    }

    // 添加内存信息到JSON对象
    cJSON_AddNumberToObject(root, "Memory", mqtt_app_heartbeat.total_memory);
    cJSON_AddNumberToObject(root, "AvailableMemory", mqtt_app_heartbeat.free_memory);
    
    // 添加磁盘信息到JSON对象
    cJSON_AddNumberToObject(root, "TotalSize", mqtt_app_heartbeat.disk_size);
    cJSON_AddNumberToObject(root, "AvailableFreeSpace", mqtt_app_heartbeat.free_disk_size);
    
    // 添加CPU使用率（保留2位小数，使用字符串格式避免精度问题）
    char cpu_rate_str[16];
    float cpu_rate_rounded = ((int)(mqtt_app_heartbeat.cpu_rate * 100 + 0.5)) / 100.0;
    snprintf(cpu_rate_str, sizeof(cpu_rate_str), "%.2f", cpu_rate_rounded);
    cJSON_AddStringToObject(root, "CpuRate", cpu_rate_str);

    // 添加温度信息
    cJSON_AddNumberToObject(root, "Temperature", mqtt_app_heartbeat.temperature);
    
    // 添加电池信息
    cJSON_AddNumberToObject(root, "Battery", mqtt_app_heartbeat.battery);

    // 添加网络信息
    cJSON_AddStringToObject(root, "IP", mqtt_app_heartbeat.local_ip);
    
    // 添加系统运行时间
    cJSON_AddNumberToObject(root, "Uptime", mqtt_app_heartbeat.uptime);
    
    // 添加时间戳
    cJSON_AddNumberToObject(root, "Time", mqtt_app_heartbeat.utc_time);
    
    // 添加网络延迟
    cJSON_AddNumberToObject(root, "Delay", mqtt_app_heartbeat.delay);

    // 添加设备模块信息
    cJSON_AddStringToObject(root, "Module", mqtt_app_heartbeat.module);

    // 添加版本信息
    cJSON_AddStringToObject(root, "Version", mqtt_app_heartbeat.version);
    
    // 将JSON对象转换为字符串
    char *str = NULL;
    str = cJSON_PrintUnformatted(root);
    if(str == NULL){
        HLK_LOG_ERR("Failed to allocate memory for JSON formatted.");
        return -1;
    }
    //PRF("#### %s\n", str);

    // 通过MQTT发布心跳包到指定主题
    ret = hlk_mqtt_publish(mqtt_topic_type_table[TOPIC_PING_POST].topic, QOS0, str, strlen(str));
    if (ret == SUCCESS) {
        HLK_LOG_INFO("mqtt ping publish ok\r\n");
    } else {
        HLK_LOG_ERR("mqtt ping publish fail[%d]\r\n", ret);
    }

    // 释放JSON对象和字符串内存
    if (root) {
        cJSON_Delete(root);
        root = NULL;
    }
    if (str) {
        free(str);
        str = NULL;
    }

    return 0;
}

/******************************************************************************
 * 函数名    : mqtt_topic_type_init
 * 功能描述  : 初始化所有MQTT主题字符串
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 遍历主题类型表，将格式化字符串转换为实际的MQTT主题
 *            使用设备的productKey和deviceName替换格式字符串中的占位符
 ******************************************************************************/
void mqtt_topic_type_init()
{
    int topic_type;

    // 遍历主题类型表，初始化所有主题字符串
    for(topic_type = 0; topic_type < N(mqtt_topic_type_table); topic_type++){
        if(mqtt_topic_type_table[topic_type].format != NULL){
            // 使用hlk_topic_make函数生成实际的主题字符串
            hlk_topic_make(mqtt_topic_type_table[topic_type].topic, 
                          mqtt_topic_type_table[topic_type].format);
            HLK_LOG_INFO("mqtt_topic_type_table[%d].topic:%s\n", topic_type, 
                mqtt_topic_type_table[topic_type].topic);
        }
        else{
            HLK_LOG_ERR("mqtt_topic_type_table format is NULL!!!!\n");
        }
    }

    return;
}

/******************************************************************************
 * 函数名    : messageArrived
 * 功能描述  : MQTT消息到达回调函数（通用消息处理）
 * 输入参数  : data - 接收到的消息数据结构指针
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 此函数为MQTT库的消息回调函数，当有消息到达时被调用
 *            目前为空实现，具体消息处理在各专用处理函数中完成
 ******************************************************************************/
void messageArrived(MessageData *data)
{
    HLK_LOG_INFO("messageArrived %d %.*s payloadlen %zu payload %.*s\n", 
        data->topicName->lenstring.len, 
        data->topicName->lenstring.len, data->topicName->lenstring.data, 
        data->message->payloadlen, 
        (int)data->message->payloadlen, (char *)data->message->payload);
    // 注释掉的调试信息，用于打印接收到的主题和消息内容
    // MESSAGE_PRINT("Message arrived on topic %.*s: %.*s\n", data->topicName->lenstring.len, data->topicName->lenstring.data,
    //        data->message->payloadlen, data->message->payload);
}

/******************************************************************************
 * 函数名    : hlk_mqtt_handle_app
 * 功能描述  : 处理来自APP端的MQTT消息
 * 输入参数  : pdata - 消息数据结构指针
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 用于接收和解析手机APP发送给设备的控制指令
 *            通过MQTT主题 "sys/{productKey}/{deviceName}/thing/property/set"
 ******************************************************************************/
static void hlk_mqtt_handle_app(MessageData *pdata)
{
    int len;
    char *str = NULL;
    
    // 获取消息长度和内容
    len = pdata->message->payloadlen;
    str = pdata->message->payload;
    
    // 打印接收到的APP消息内容（调试用）
    HLK_LOG_INFO("%s\n", str);   
    
    //解析这个json 把InputData中的Name和Value提取出来
    cJSON *root = cJSON_Parse(str);
    if (root == NULL) {
        HLK_LOG_ERR("cJSON_Parse error\n");
        return;
    }

    cJSON *inputData = cJSON_GetObjectItem(root, "InputData");
    if (!inputData) {
        HLK_LOG_ERR("InputData not found\n");
        cJSON_Delete(root);
        return;
    }
    
    //提取Name的值
    cJSON *nameObj = cJSON_GetObjectItem(inputData, "Name");
    if (!nameObj || !cJSON_IsString(nameObj)) {
        HLK_LOG_ERR("Name not found or not string\n");
        cJSON_Delete(root);
        return;
    }
    
    char *name = nameObj->valuestring;
    HLK_LOG_INFO("Channel name: %s\n", name);

    //处理来自 DataPointsDown 的信息
    if (strcmp(name, "DataPointsDown") == 0) {
        //处理来自 DataPointsDown 的信息
        //云端下发信息 有两种情况 一个是设置 一个是采集
        hlk_mqtt_handle_data_points_down(root);
        cJSON_Delete(root);
        return;
    }

    // 创建推送数据的JSON对象
    cJSON *pushData = cJSON_CreateObject();
    cJSON_AddStringToObject(pushData, "type", "cloud_data");
    cJSON_AddStringToObject(pushData, "channel", name);
    cJSON_AddItemToObject(pushData, "inputData", cJSON_Duplicate(inputData, 1));
    cJSON_AddNumberToObject(pushData, "timestamp", zig_get_timestamp());
    
    // 将完整的原始消息也包含进去
    cJSON_AddStringToObject(pushData, "raw_message", str);

    {
        // 构建发送给ser2net的JSON消息
        cJSON *ipcMessage = cJSON_CreateObject();
        cJSON *params = cJSON_CreateObject();
        cJSON *data = cJSON_CreateObject();
        
        cJSON_AddStringToObject(data, "Name", name);
        if (cJSON_IsString(cJSON_GetObjectItem(inputData, "Value"))) {
            cJSON_AddStringToObject(data, "Value", cJSON_GetObjectItem(inputData, "Value")->valuestring);
        } else if (cJSON_IsNumber(cJSON_GetObjectItem(inputData, "Value"))) {
            cJSON_AddNumberToObject(data, "Value", cJSON_GetObjectItem(inputData, "Value")->valueint);
        } else if (cJSON_IsBool(cJSON_GetObjectItem(inputData, "Value"))) {
            cJSON_AddBoolToObject(data, "Value", cJSON_GetObjectItem(inputData, "Value")->valueint);
        }
        
        cJSON_AddStringToObject(params, "channel", name);
        cJSON_AddItemToObject(params, "data", data);
        
        cJSON_AddStringToObject(ipcMessage, "method", "send_channel_data");
        cJSON_AddItemToObject(ipcMessage, "params", params);
        cJSON_AddNumberToObject(ipcMessage, "id", 1);
        
        char *ipcMessageStr = cJSON_PrintUnformatted(ipcMessage);
        if (ipcMessageStr) {
            // 通过IPC向监听TRANSDOWN通道的特定客户端发送数据
            hi_link_ipc_send_to_channel_listeners(name, ipcMessageStr);
            free(ipcMessageStr);
        }
        cJSON_Delete(ipcMessage);
    }

    //设置通道数据
    if (strcmp(name, "SSID") == 0 && cJSON_IsString(cJSON_GetObjectItem(inputData, "Value"))) {
        //设置WIFI的SSID
        char *value = cJSON_GetObjectItem(inputData, "Value")->valuestring;
        HLK_LOG_INFO("name: %s, value: %s\n", name, value);
        
        // 推送数据到SSE
        char *pushDataStr = cJSON_PrintUnformatted(pushData);
        if (pushDataStr) {
            push_data_to_sse(name, pushDataStr);
            free(pushDataStr);
        }

    } else if (strcmp(name, "Channel") == 0 && cJSON_IsNumber(cJSON_GetObjectItem(inputData, "Value"))) {
        //设置WIFI的信道
        int value = cJSON_GetObjectItem(inputData, "Value")->valueint;
        HLK_LOG_INFO("name: %s, value: %d\n", name, value);
        
        // 推送数据到SSE
        char *pushDataStr = cJSON_PrintUnformatted(pushData);
        if (pushDataStr) {
            push_data_to_sse(name, pushDataStr);
            free(pushDataStr);
        }

    } else if (strcmp(name, "TRANSDOWN") == 0 && cJSON_IsString(cJSON_GetObjectItem(inputData, "Value"))) {
        //设置透传下行
        char *value = cJSON_GetObjectItem(inputData, "Value")->valuestring;
        HLK_LOG_INFO("name: %s, value: %s\n", name, value);
        
        // 推送数据到SSE
        char *pushDataStr = cJSON_PrintUnformatted(pushData);
        if (pushDataStr) {
            push_data_to_sse(name, pushDataStr);
            free(pushDataStr);
        }
    } else if (strcmp(name, "DataPointsDown") == 0 && cJSON_IsString(cJSON_GetObjectItem(inputData, "Value"))) {
        //采集数据下发数据属性通道
        //数据格式device=devname&point1=xx&point2=xx&point3=xx&device=devname2&point1=xx&point2=xx&point3=xx
        char *value = cJSON_GetObjectItem(inputData, "Value")->valuestring;
        //modbus_collector(value);  //传给modbus采集器处理
    } else {

        //云端可以自定义通道名称，此处可以处理自定义通道名称的逻辑
        //通道数据类型不定 怎么转发到 透传中

        HLK_LOG_ERR("value error\n");
        
        // 即使是未知类型，也推送到SSE
        char *pushDataStr = cJSON_PrintUnformatted(pushData);
        if (pushDataStr) {
            push_data_to_sse(name, pushDataStr);
            free(pushDataStr);
        }
    }
    
    cJSON_Delete(pushData);
    cJSON_Delete(root);
}

/******************************************************************************
 * 函数名    : hlk_mqtt_handle_set
 * 功能描述  : 处理来自云端的设备设置消息
 * 输入参数  : pdata - 消息数据结构指针
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 用于接收和处理云端下发的设备配置和设置指令
 *            通过MQTT主题 "sys/{productKey}/{deviceName}/thing/service/post"
 ******************************************************************************/
static void hlk_mqtt_handle_set(MessageData *pdata)
{
    // TODO: 实现云端设置消息的解析和处理逻辑
    // 例如：设备参数配置、工作模式切换等
    HLK_LOG_INFO("hlk_mqtt_handle_set %d %.*s payloadlen %zu payload %.*s\n", 
        pdata->topicName->lenstring.len, 
        pdata->topicName->lenstring.len, pdata->topicName->lenstring.data, 
        pdata->message->payloadlen, 
        (int)pdata->message->payloadlen, (char *)pdata->message->payload);

}

/******************************************************************************
 * 函数名    : hlk_mqtt_get_state (已注释)
 * 功能描述  : 获取MQTT连接状态
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : g_mqtt_flag - MQTT连接状态标志
 * 说明      : 此函数已被注释，用于查询当前MQTT连接状态
 ******************************************************************************/
/*int hlk_mqtt_get_state(void)
{
    return g_mqtt_flag;
}*/

/******************************************************************************
 * 函数名    : hlk_mqtt_reset_reply
 * 功能描述  : 发送设备重置操作的回复消息
 * 输入参数  : reset  - 重置状态（0-成功 非0-失败）
 *            reply  - 回复状态
 *            msg_Id - 消息ID（与接收到的重置请求ID对应）
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 当设备收到重置指令后，通过此函数向云端发送执行结果
 ******************************************************************************/
void hlk_mqtt_reset_reply(int reset, int reply, long long msg_Id)
{
    // 获取当前系统时间戳
    uint32_t __time = hlk_sntp_time_get();
    HLK_LOG_INFO("__time = %d\n", __time);
    
    // 创建JSON文档对象用于构建回复消息
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *arr = yyjson_mut_obj(doc);
    
    // 添加重置回复消息的各个字段
    yyjson_mut_obj_add_uint(doc, root, "Reset", reset);    // 重置状态
    yyjson_mut_obj_add_uint(doc, root, "Id", msg_Id);      // 消息ID
    yyjson_mut_obj_add_uint(doc, root, "Time", __time);    // 时间戳
    yyjson_mut_obj_add_uint(doc, root, "Reply", reply);    // 回复状态
    
    // 将JSON对象转换为字符串
    char *str = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, NULL);
    HLK_LOG_INFO("%s\n", str);
    
    if (str){
        // 通过重置主题发布回复消息
        hlk_mqtt_publish(mqtt_topic_type_table[TOPIC_RESET].topic, QOS0, str, strlen(str));
        free(str);               // 释放字符串内存
        yyjson_mut_doc_free(doc); // 释放JSON文档内存
    }
    str = NULL;
}

/******************************************************************************
 * 函数名    : hlk_mqtt_handle_reset
 * 功能描述  : 处理来自云端的设备重置指令
 * 输入参数  : data - 接收到的重置消息数据
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 解析云端下发的重置指令，根据指令内容执行相应的重置操作
 *            支持配置清除和设备重启两种重置模式
 ******************************************************************************/
static void hlk_mqtt_handle_reset(MessageData *data)
{
    int len = data->message->payloadlen;
    char *str = data->message->payload;
    long long reset_msg_Id;
    
    HLK_LOG_INFO("%s\n", str);

    // 解析接收到的JSON格式重置指令
    yyjson_doc *doc = yyjson_read(str, len, 0);
    if (!doc){
        HLK_LOG_ERR("Error parsing JSON\n");
        return;
    }
    
    // 获取JSON根对象
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)){
        HLK_LOG_ERR("Root is not an object\n");
        goto exit;
    }
    
    // 解析配置清除标志
    yyjson_val *configClean_val = yyjson_obj_get(root, "ConfigClean");
    if (configClean_val == NULL){
        HLK_LOG_ERR("yyjson_obj_get ConfigClean error\n");
        goto exit;
    }
    
    // 解析设备重置标志
    yyjson_val *deviceReset_val = yyjson_obj_get(root, "DeviceReset");
    if (configClean_val == NULL){
        HLK_LOG_ERR("yyjson_obj_get DeviceReset error\n");
        goto exit;
    }
    
    // 获取消息ID，用于回复确认
    yyjson_val *valueid = yyjson_obj_get(root, "Id");
    if (valueid == NULL || !yyjson_is_num(valueid)){
        HLK_LOG_ERR("yyjson_obj_get Id error\n");
    }else{
        reset_msg_Id = yyjson_get_sint(valueid);
        HLK_LOG_INFO("reset_msg_Id %lld \r\n", reset_msg_Id);
    }

    // 声明重置回复函数
    void hlk_mqtt_reset_reply(int reset, int reply, long long msg_Id);
    
    // 检查是否需要执行设备重置
    if (yyjson_get_sint(deviceReset_val))
    {
        /* 根据配置清除标志决定重置类型 */
        if (yyjson_get_sint(configClean_val)){
            hlk_mqtt_reset_reply(0, 1, reset_msg_Id);  // 配置清除重置
        }else{
            hlk_mqtt_reset_reply(0, 0, reset_msg_Id);  // 普通重启
        }
            
        // 断开MQTT连接
        MQTTDisconnect(hlk_iot.client);
        app_msleep(2000);  // 等待2秒确保消息发送完成
        
        if (yyjson_get_sint(configClean_val))
        {
            /* 清除WiFi配网信息 */
            //user_wificonfig_clean();  // 根据实际情况启用
        }
        
        /* 执行设备重启 */
        #if defined(HLK_PRODUCT_WR10) || defined(HLK_PRODUCT_7628)
        app_reboot();
        #elif defined (HLK_PRODUCT_RM50)
        HLK_LOG_INFO("RM50 Upgrade ");
        #endif
    }
    
exit:
    if (doc)
    {
        yyjson_doc_free(doc);  // 释放JSON解析器内存
    }
}

/******************************************************************************
 * 函数名    : hlk_mqtt_handle_ping_reply
 * 功能描述  : 处理云端对心跳包的回复，同步流量控制设置
 * 输入参数  : data - 心跳回复消息数据
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 解析云端返回的流量限制信息，用于控制设备的消息发送频率
 *            包含：是否启用限制、已发送消息数、最大消息数限制等
 ******************************************************************************/
static void hlk_mqtt_handle_ping_reply(MessageData *data)
{
    // 获取心跳回复消息内容
    int len = data->message->payloadlen;
    char *str = data->message->payload;
    HLK_LOG_INFO("%s\n", str);
    static int time_sync = 0;

    // 定义JSON解析相关变量
    cJSON *root = NULL, *item = NULL, *flag = NULL, *max_count = NULL, *server_time = NULL;
    
    // 解析JSON格式的心跳回复
    root = cJSON_Parse(str);
    if (root == NULL)
    {
        HLK_LOG_ERR("cJSON_Parse error\n");
        goto exit;
    }
    
    // 解析是否启用流量限制标志
    flag = cJSON_GetObjectItem(root, "HasLimit");
    if (flag == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem HasLimit error\n");
        goto exit;
    }
    
    // 解析已发送消息计数
    item = cJSON_GetObjectItem(root, "AlreadyMesCount");
    if (item == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem AlreadyMesCount error\n");
        goto exit;
    }
    
    // 解析消息限制的最大值
    max_count = cJSON_GetObjectItem(root, "MesLimitMin");
    if (max_count == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem MesLimitMin error\n");
        goto exit;
    }


    //服务器时间同步
    unsigned long long utc_time;
    server_time = cJSON_GetObjectItem(root, "ServerTime");
    if (server_time == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem ServerTime error\n");
        goto exit;
    }
    utc_time = server_time->valuedouble;
    if (time_sync == 0)
    {
        zig_set_timesync(utc_time);
        // 使用Zig实现的system()时区设置，直接调用system避免setenv问题
        zig_set_timezone_system("CST-8");
        time_sync = 1;
    }
    // 设置消息流量限制（函数已注释）
    //hlk_mqtt_set_msg_limit(flag->valueint, item->valueint, max_count->valueint);

    sharedData.connect_status = MQTT_CONNECT_STATUS_CONNECTED;            // 连接状态设为2
    set_cloud_status(MQTT_CONNECT_STATUS_CONNECTED);
exit:
    if (root)
    {
        cJSON_Delete(root);  // 释放JSON对象内存
    }
}

/******************************************************************************
 * 函数名    : parse_http_url
 * 功能描述  : 解析HTTP URL，提取主机名、端口号和路径信息
 * 输入参数  : url  - 完整的HTTP URL字符串
 * 输出参数  : host - 解析出的主机名
 *            port - 解析出的端口号（默认80）
 *            path - 解析出的路径部分
 * 返回值    : 无
 * 说明      : 用于OTA升级时解析下载URL，分离出连接所需的各个组件
 *            支持格式：http://hostname:port/path 或 http://hostname/path
 ******************************************************************************/
void parse_http_url(const char *url, char *host, int *port, char *path) 
{
    // 设置默认HTTP端口
    *port = 80;
    HLK_LOG_INFO("url = %s\n", url);
    
    // 查找协议分隔符 "://"
    const char *start = strstr(url, "://");
    if (start == NULL) {
        HLK_LOG_ERR("Invalid URL\n");
        return;
    }

    // 跳过协议部分，开始解析主机部分
    start += 3; // 跳过 "://"
    const char *end = strchr(start, '/');
    if (end == NULL) {
        HLK_LOG_ERR("Invalid URL\n");
        return;
    }

    // 提取主机名部分
    strncpy(host, start, end - start);
    host[end - start] = '\0';

    // 检查是否包含端口号
    char *colon = strchr(host, ':');
    if (colon != NULL) {
        *port = atoi(colon + 1);  // 解析端口号
        *colon = '\0';            // 截断主机名，去掉端口部分
    }

    // 复制路径部分
    strcpy(path, end);
}

/******************************************************************************
 * 函数名    : write_callback
 * 功能描述  : libcurl下载数据的写入回调函数
 * 输入参数  : ptr    - 接收到的数据缓冲区指针
 *            size   - 每个数据块的字节数
 *            nmemb  - 数据块的数量
 *            stream - 文件流指针（用于写入数据）
 * 输出参数  : 无
 * 返回值    : 实际写入的字节数
 * 说明      : OTA升级时，libcurl通过此回调函数将下载的固件数据写入本地文件
 *            全局变量g_wirtten用于统计总写入字节数
 ******************************************************************************/
size_t write_callback(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t expected_bytes;
    size_t wirtten = 0;
    HLK_LOG_INFO("write_callback size:%d, nmemb:%d\n", size, nmemb);
    HLK_LOG_INFO("ptr:%s\n", ptr);

    // 检查输入参数的有效性
    if (!ptr || !stream || size == 0 || nmemb == 0) {
        HLK_LOG_ERR("write_callback: 无效的输入参数\n");
        return 0;  // 返回0告知libcurl出现错误
    }

    // 第一次写入时跳过前2个字节
    if (g_curl_schedule == 0)
    {
        wirtten = fwrite((char *)ptr + 2, size, nmemb-2, stream);
        g_curl_schedule = 1;
        expected_bytes = size * (nmemb-2);
    } else {
        wirtten = fwrite((char *)ptr, size, nmemb, stream);
        expected_bytes = size * nmemb;
    }

    // 检查写入是否成功
    if (wirtten != expected_bytes) {
        // 写入失败或部分写入
        if (ferror(stream)) {
            HLK_LOG_ERR("write_callback: 文件写入错误，可能原因：磁盘空间不足或I/O错误\n");
        } else if (feof(stream)) {
            HLK_LOG_ERR("write_callback: 意外遇到文件结尾\n");
        } else {
            HLK_LOG_ERR("write_callback: 部分写入，期望写入 %zu 项，实际写入 %zu 项\n", nmemb, wirtten);
        }
        
        // 强制刷新缓冲区，确保已写入的数据保存到磁盘
        if (fflush(stream) != 0) {
            HLK_LOG_ERR("write_callback: 刷新文件缓冲区失败\n");
        }
        
        // 返回0告知libcurl出现错误，这会中断下载
        return 0;
    }

    // 写入成功，累计写入字节数
    g_wirtten += expected_bytes * size;

    return nmemb;  // 返回成功写入的项数
}

/******************************************************************************
 * 函数名    : progress_callback
 * 功能描述  : libcurl下载进度回调函数
 * 输入参数  : clientp - 客户端数据指针（未使用）
 *            dltotal - 预期下载总字节数
 *            dlnow   - 当前已下载字节数
 *            ultotal - 预期上传总字节数（未使用）
 *            ulnow   - 当前已上传字节数（未使用）
 * 输出参数  : 无
 * 返回值    : 0-继续下载 非0-中断下载
 * 说明      : 在OTA升级下载过程中显示下载进度百分比
 ******************************************************************************/
int progress_callback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    if (dltotal > 0) {
        // 计算并打印下载进度百分比
        HLK_LOG_INFO("\r\n=================%.0f%%=================\r\n", dlnow / dltotal * 100);
    }
    return 0;  // 返回0继续下载
}

/******************************************************************************
 * 函数名    : set_post_headers
 * 功能描述  : 设置OTA升级HTTP POST请求的头部信息
 * 输入参数  : 无
 * 输出参数  : p_m_h_post_headers - 填充完成的HTTP头部结构体
 * 返回值    : 无
 * 说明      : 生成包含认证信息的HTTP头部，用于OTA文件下载时的身份验证
 *            包含语言、ID、随机数、时间戳、访问ID和数字签名等字段
 ******************************************************************************/
void set_post_headers(M_HTTP_POST_HEADERS_S *p_m_h_post_headers)
{
    unsigned long long current_time;
    struct timespec spec;
    
    // 获取当前时间戳（毫秒级）
    #ifdef HLK_PRODUCT_WR10
    clock_gettime(CLOCK_REALTIME, &spec);
    current_time = spec.tv_sec * 1000 + spec.tv_nsec / 1000000;
    #elif defined(HLK_PRODUCT_7628)
    current_time = (unsigned long long)zig_get_timestamp()*1000;

    #endif

    // 生成随机数用于请求唯一标识
    int iRandomNumber = 0;
    srand(zig_get_timestamp());       // 设置随机数种子
    iRandomNumber = rand();

    // 构建MD5签名所需的字符串
    char acStringA[128] = {0};
    char acStringB[128] = "&key=X9jrVZ8RRE2fXpRw";  // 固定密钥后缀
    char acUnmd5[256] = {0};
    
    // 构建待加密字符串：accessId + nonceStr + timeStamp
    sprintf(acStringA, "accessId=%s&nonceStr=H8NDPR&timeStamp=%llu", 
            mqtt_user_cert.deviceName, current_time);
    sprintf(acUnmd5, "%s%s", acStringA, acStringB);
 
    // 执行MD5加密生成数字签名
    char acFinalKey[64] = {0};
    char acKey[16] = {0};
    MD5_CTX md5;
    memset(&md5, 0, sizeof(MD5_CTX));
    MD5Init(&md5);
    MD5Update(&md5, (unsigned char*)acUnmd5, strlen(acUnmd5));
    MD5Final(&md5, (unsigned char*)acKey);
    
    // 将MD5结果转换为十六进制字符串
    conver_hex2str((char *)acKey, sizeof(acKey), (char *)acFinalKey, sizeof(acFinalKey));

    // 构建HTTP头部各字段
    sprintf(p_m_h_post_headers->acLng, "Lng: %s", "cn");                           // 语言设置
    sprintf(p_m_h_post_headers->acId, "Id: %s_%d", MODULE_TYPE, iRandomNumber);    // 请求ID
    sprintf(p_m_h_post_headers->acNonce, "Nonce: %d", iRandomNumber);              // 随机数
    sprintf(p_m_h_post_headers->acTimeStamp, "TimeStamp: %llu", current_time);      // 时间戳
    sprintf(p_m_h_post_headers->acAccessId, "AccessId: %s", mqtt_user_cert.deviceName); // 访问ID
    sprintf(p_m_h_post_headers->acSignature, "Signature: %s", acFinalKey);         // 数字签名

    return;
}

/******************************************************************************
 * 函数名    : set_post_body
 * 功能描述  : 设置OTA分段下载的HTTP POST请求体
 * 输入参数  : start - 下载起始位置（字节偏移）
 *            size  - 本次下载的数据大小
 * 输出参数  : p_m_h_post_body - 填充完成的HTTP请求体结构
 * 返回值    : 无
 * 说明      : 用于OTA大文件分段下载，指定每次下载的数据范围
 *            请求体格式：Code=msgid_fid&Start=起始位置&Size=数据大小
 ******************************************************************************/
void set_post_body(M_HTTP_POST_BODY_S *p_m_h_post_body, int start, int size)
{
    // 构建POST请求体，包含消息ID、文件ID、起始位置和大小
    sprintf(p_m_h_post_body->acBody, "Code=%d_%d&Start=%d&Size=%d", 
            g_ota_msgid, g_ota_fid, start, size);
    return;
}

/******************************************************************************
 * 函数名    : consolidated_file_removes_checksum
 * 功能描述  : 合并文件时移除校验和（跳过文件头部2字节）
 * 输入参数  : targetFile - 目标文件指针（最终的升级文件）
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 将临时下载文件的内容（跳过前2字节校验和）追加到目标升级文件中
 *            这是OTA分段下载过程中的文件合并操作
 ******************************************************************************/
void consolidated_file_removes_checksum(FILE *targetFile)
{
    FILE *sourceFile;
    char buffer[BUFFER_SIZE];
    size_t bytesRead;

    // 打开临时下载文件
    sourceFile = fopen(SYSUPGRADE_BIN_PATH_TMP, "rb");
    if (sourceFile == NULL) {
        HLK_LOG_ERR("fopen %s fail\n", SYSUPGRADE_BIN_PATH_TMP);
        return;
    }

    // 跳过文件开头的2字节校验和
    fseek(sourceFile, 2, SEEK_SET);

    // 逐块读取源文件内容并写入目标文件
    while ((bytesRead = fread(buffer, sizeof(char), BUFFER_SIZE, sourceFile)) > 0) {
        fwrite(buffer, sizeof(char), bytesRead, targetFile);
    }

    // 确保数据写入磁盘
    if(targetFile)
        fflush(targetFile);
        
    // 关闭源文件
    if(sourceFile){
        fclose(sourceFile);
    }

    return;
}

/******************************************************************************
 * 函数名    : hlk_ota_write_version
 * 功能描述  : 将OTA版本信息写入本地文件
 * 输入参数  : filename - 版本信息文件路径
 *            info     - 版本信息结构体指针
 *            pro      - 升级进度状态
 * 输出参数  : 无
 * 返回值    : 0-成功 非0-失败
 * 说明      : 用于保存OTA升级过程中的版本信息和进度状态
 *            便于设备重启后继续升级流程或上报升级结果
 ******************************************************************************/
int hlk_ota_write_version(const char *filename, const VersionInfo *info, int pro)
{
    FILE *fp_;
    fp_ = fopen(filename, "wb");
    if (!fp_){
        HLK_LOG_ERR("open %s failed! \r\n", filename);
        return -1;
    }
    
    // 复制版本信息并设置升级进度
    VersionInfo tmp = *info;
    tmp.progress = pro;  // 设置升级流程状态
    
    // 将版本信息写入文件
    int success = (fwrite(&tmp, sizeof(tmp), 1, fp_) == 1);
    HLK_LOG_INFO("write sucess\r\n");
    fclose(fp_);
    return success ? 0 : -1;
}

/******************************************************************************
 * 函数名    : hlk_ota_read_version
 * 功能描述  : 从本地文件读取OTA版本信息
 * 输入参数  : filename - 版本信息文件路径
 * 输出参数  : out      - 读取到的版本信息结构体
 * 返回值    : 0-成功 非0-失败
 * 说明      : 读取之前保存的OTA升级信息，用于设备重启后检查升级状态
 *            可获取版本号、消息ID、升级进度等信息
 ******************************************************************************/
int hlk_ota_read_version(const char *filename, VersionInfo *out)
{
    FILE *fp_ = fopen(filename, "rb");
    if (!fp_)
    {
        HLK_LOG_ERR("open %s failed! \r\n", filename);
        return -1;
    }
    
    // 读取版本信息结构体
    int success = (fread(out, sizeof(VersionInfo), 1, fp_) == 1);
    HLK_LOG_INFO("\r\n==Read== Version:%s\r\nmsgid:%d\r\n", out->version, out->msgid);

    fclose(fp_);
    return success ? 0 : -1;
}

/******************************************************************************
 * 函数名    : hlk_delete_file
 * 功能描述  : 删除指定路径的文件
 * 输入参数  : path - 要删除的文件路径
 * 输出参数  : 无
 * 返回值    : 0-成功 非0-失败
 * 说明      : 封装标准库的remove函数，用于清理临时文件和升级缓存文件
 ******************************************************************************/
int hlk_delete_file(const char *path)
{
    return (remove(path) == 0) ? 0 : -1;
}

/******************************************************************************
 * 函数名    : hlk_mqtt_report_version
 * 功能描述  : 向云端上报OTA升级版本信息和进度状态
 * 输入参数  : code   - 升级状态码（成功/失败原因）
 *            step   - 升级进度百分比（0-100）
 *            msg_id - 消息ID（与升级请求对应）
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 在OTA升级过程中向云端实时上报升级进度和状态
 *            支持多种错误类型的消息回馈，便于云端监控升级状态
 ******************************************************************************/
void hlk_mqtt_report_version(int code, int step, int msg_id)
{
    cJSON *root = NULL;
    
    // 创建JSON对象用于构建上报消息
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "Code", code);  // 状态码
    
    // 根据不同的状态码设置相应的消息内容
    switch (code)
    {
    case HLK_OTA_NO_ERR:  // 升级正常
        if(step == 100)
        {
            cJSON_AddStringToObject(root, "Message", "Upgrade completed");  // 升级完成
        }
        else
        {
            cJSON_AddStringToObject(root, "Message", "Upgrading");          // 正在升级
        }
        break;
        
    case HLK_OTA_VER_ERR:  // 版本不匹配错误
        cJSON_AddStringToObject(root, "Message", "Version No Match!");
        break;
        
    case HLK_OTA_FLASH_CHECK_ERR:  // Flash校验失败
        cJSON_AddStringToObject(root, "Message", "Flash Check Fail!");
        break;
        
    case HLK_OTA_URL_ERR:  // URL错误
        cJSON_AddStringToObject(root, "Message", "Url Can Not Use!");
        break;
        
    default:  // 其他未知错误
        cJSON_AddStringToObject(root, "Message", "Some Error Occur!");
        break;
    }

    // 添加升级进度和相关信息
    cJSON_AddNumberToObject(root, "Step", step);              // 升级进度百分比
    cJSON_AddNumberToObject(root, "MsgId", msg_id);           // 消息ID
    cJSON_AddStringToObject(root, "Module", MODULE_TYPE);     // 模块类型

    // 获取并添加当前设备版本信息
    char _version[128] = {0};
    get_version_info(_version);
    cJSON_AddStringToObject(root, "Version", _version);

    // 将JSON对象转换为字符串
    char *str = cJSON_PrintUnformatted(root);
    HLK_LOG_INFO("%s\n", str);

    // 释放JSON对象
    if(root)
        cJSON_Delete(root);
        
    // 通过MQTT发布升级状态消息到云端
    int ret = hlk_mqtt_publish(mqtt_topic_type_table[TOPIC_UPGRADE_REPLY].topic, QOS0, str, strlen(str));
    if(ret == SUCCESS){
        HLK_LOG_INFO("mqtt upgrade version publish ok\n");
    }else{
        HLK_LOG_ERR("mqtt upgrade version publish fail\n");
    }
    
    // 释放字符串内存
    if(str)
        free(str);
}

/******************************************************************************
 * 函数名    : hlk_ota_check_version
 * 功能描述  : 检查设备启动时的OTA升级状态
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 设备启动时调用，检查是否有未完成的OTA升级
 *            比较当前版本与缓存版本，判断升级是否成功，并上报结果给云端
 ******************************************************************************/
void hlk_ota_check_version(void)
{
    VersionInfo read_verinfo;
    char version_now[64];
    
    // 尝试读取缓存的版本升级信息
    if (hlk_ota_read_version(SYSUPGRADE_MSGID_PATH, &read_verinfo) < 0){
        // 读取文件失败或文件不存在，说明没有待处理的升级
        HLK_LOG_ERR("Read %s err\r\n", SYSUPGRADE_MSGID_PATH);
    } else {
        // 文件存在，检查版本号和消息ID
        if (read_verinfo.msgid) {
            get_version_info(version_now); // 获取当前运行的版本号
            HLK_LOG_INFO("version_now:%s\r\n read_verinfo:%s\r\n", version_now, read_verinfo.version);

            // 比较当前版本与升级目标版本
            if (strcmp(version_now, read_verinfo.version) == 0 && read_verinfo.progress == 1){
                // 版本匹配且升级流程标志为1，表示升级成功
                hlk_mqtt_report_version(HLK_OTA_NO_ERR, 100, read_verinfo.msgid);
            } else {
                // 版本不匹配或升级流程异常，表示升级失败
                hlk_mqtt_report_version(HLK_OTA_FLASH_CHECK_ERR, 0, read_verinfo.msgid);
            }
            
            // 删除版本缓存文件，避免重复检查
            hlk_delete_file(SYSUPGRADE_MSGID_PATH);
        }
    }
}

/******************************************************************************
 * 函数名    : hlk_ota_http_child_process
 * 功能描述  : 子进程执行OTA下载的具体逻辑
 * 输入参数  : server_name - 服务器主机名
 *            path        - 下载文件的URL路径
 *            pipe_fd     - 管道文件描述符，用于向父进程发送结果
 * 输出参数  : 无
 * 返回值    : 无（子进程直接exit）
 * 说明      : 在子进程中执行实际的HTTP下载逻辑
 ******************************************************************************/
static void hlk_ota_http_child_process(char *server_name, char *path, int pipe_fd)
{
    CURL *curl;
    FILE *fp, *fp_bin;
    CURLcode res = CURLE_OK;
    float percent;
    ota_download_result_t result = {0};
    
    // 构建完整的下载URL
    char acUrl[1024] = {0}; 
    sprintf(acUrl, "http://%s%s", server_name, path);
    
    HLK_LOG_INFO("Child process: acUrl:%s\n", acUrl);

    // 初始化libcurl全局环境
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // 清除缓存
    system("echo 1 > /proc/sys/vm/drop_caches");
    system("echo 2 > /proc/sys/vm/drop_caches");
    system("echo 3 > /proc/sys/vm/drop_caches");

    curl = curl_easy_init();
    if (!curl) {
        result.result_code = CURLE_FAILED_INIT;
        strcpy(result.error_msg, "Failed to initialize curl");
        write(pipe_fd, &result, sizeof(result));
        curl_global_cleanup();
        exit(1);
    }

    HLK_LOG_INFO("Child process: start download==========>\r\n");
    
    // 配置分段下载参数
    int segment_size = 4096*1024;  // 每段1M
    int total_size = g_ota_size;
    // 准备HTTP头部和请求体
    struct curl_slist *headers = NULL;
    M_HTTP_POST_HEADERS_S m_h_post_headers = {0};
    M_HTTP_POST_BODY_S m_h_post_body = {0};

    // 打开最终的升级文件
    fp_bin = fopen(SYSUPGRADE_BIN_PATH, "wb");
    if (!fp_bin) {
        result.result_code = CURLE_WRITE_ERROR;
        strcpy(result.error_msg, "Failed to open upgrade file");
        write(pipe_fd, &result, sizeof(result));
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        exit(1);
    }

    fseek(fp_bin, 0, SEEK_SET);

    fp = fopen(SYSUPGRADE_BIN_PATH_TMP, "wb");
    if (!fp) {
        result.result_code = CURLE_WRITE_ERROR;
        strcpy(result.error_msg, "Failed to open temp file");
        write(pipe_fd, &result, sizeof(result));
        goto end;
    }

    fseek(fp, 0, SEEK_SET);
    g_wirtten = 0;
    g_curl_schedule = 0;

    // 分段下载循环
    for (int start = 0; start < total_size; start += segment_size)
    {
        curl_easy_reset(curl);
        int siDownFailFlag = 0;
        int end = start + segment_size - 1;
        int download_size = segment_size;
        
        if (end >= total_size) {
            end = total_size - 1;
            download_size = total_size - start;
        }
        
        percent = (float)(start + download_size) / total_size * 100;
        
        // 向父进程报告进度
        result.result_code = CURLE_OK;
        result.download_progress = (int)percent;
        sprintf(result.error_msg, "Downloading: %.0f%%", percent);
        write(pipe_fd, &result, sizeof(result));

    re_download:


        set_post_headers(&m_h_post_headers);
        set_post_body(&m_h_post_body, start, download_size);

        // 构建HTTP头部链表
        headers = curl_slist_append(headers, m_h_post_headers.acLng);
        headers = curl_slist_append(headers, m_h_post_headers.acId);
        headers = curl_slist_append(headers, m_h_post_headers.acNonce);
        headers = curl_slist_append(headers, m_h_post_headers.acTimeStamp);
        headers = curl_slist_append(headers, m_h_post_headers.acAccessId);
        headers = curl_slist_append(headers, m_h_post_headers.acSignature);

        // 配置CURL选项
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, m_h_post_body.acBody);
        curl_easy_setopt(curl, CURLOPT_URL, acUrl);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 1);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

        // 执行HTTP请求
        res = curl_easy_perform(curl);

        // 立即释放HTTP头部内存
        if (headers) {
            curl_slist_free_all(headers);
            headers = NULL;
        }

        if (res != CURLE_OK) {
            siDownFailFlag++;
            HLK_LOG_INFO("Child process: curl_easy_perform() failed: %d, %s\n", res, errbuf);
            
            if (siDownFailFlag <= 3) {
                HLK_LOG_INFO("Child process: Retry %d/3\n", siDownFailFlag);
                if (fp) {
                    fclose(fp);
                    fp = NULL;
                }
                sleep(1);  // 等待1秒后重试
                goto re_download;
            } else {
                HLK_LOG_ERR("Child process: Max retries reached\n");
                result.result_code = res;
                sprintf(result.error_msg, "Download failed after 3 retries: %s", errbuf);
                write(pipe_fd, &result, sizeof(result));
                if (fp) {
                    fclose(fp);
                    fp = NULL;
                }
                break;
            }
        } else {
            HLK_LOG_INFO("Child process: download segment success\n");
            g_curl_schedule = 0;
        }


        #if defined(HLK_PRODUCT_WR10)
        if (res == CURLE_OK) {
            consolidated_file_removes_checksum(fp_bin);
        }
        #endif
    }
end:
    // 确保数据写入磁盘并关闭文件
    if (fp_bin) {
        fflush(fp_bin);
        fclose(fp_bin);
    }
    if (fp) {
        fclose(fp);
        fp = NULL;
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();

    // 发送最终结果
    result.result_code = res;
    result.download_progress = (res == CURLE_OK) ? 100 : 0;
    if (res == CURLE_OK) {
        strcpy(result.error_msg, "Download completed successfully");
    } else {
        sprintf(result.error_msg, "Download failed with error: %d", res);
    }
    write(pipe_fd, &result, sizeof(result));
    
    HLK_LOG_INFO("Child process: ===================>end\r\n");
    exit(res == CURLE_OK ? 0 : 1);
}

/******************************************************************************
 * 函数名    : hlk_ota_http
 * 功能描述  : OTA升级HTTP下载入口函数（使用子进程）
 * 输入参数  : server_name - 服务器主机名
 *            path        - 下载文件的URL路径
 * 输出参数  : 无
 * 返回值    : CURL错误码（CURLE_OK表示成功）
 * 说明      : 使用子进程进行OTA固件下载，避免线程问题
 ******************************************************************************/
int hlk_ota_http(char *server_name, char *path)
{
#ifdef ENABLE_CURL
    int pipe_fds[2];
    pid_t child_pid;
    int status;
    ota_download_result_t result;
    zig_fd_set read_fds;
    zig_timeval timeout;
    int select_result;
    time_t start_time, current_time;

    HLK_LOG_INFO("Starting OTA download with child process\n");
    
    // 创建管道用于进程间通信
    if (pipe(pipe_fds) == -1) {
        HLK_LOG_ERR("Failed to create pipe: %s\n", strerror(errno));
        return CURLE_FAILED_INIT;
    }
    
    // 记录开始时间
    start_time = zig_get_timestamp();
    
    // 创建子进程
    child_pid = fork();
    if (child_pid == -1) {
        HLK_LOG_ERR("Failed to fork child process: %s\n", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return CURLE_FAILED_INIT;
    }
    
    if (child_pid == 0) {
        // 子进程：执行下载逻辑
        close(pipe_fds[0]);  // 子进程不需要读端
        hlk_ota_http_child_process(server_name, path, pipe_fds[1]);
        // 子进程在hlk_ota_http_child_process中会exit，不会执行到这里
    } else {
        // 父进程：等待子进程结果
        close(pipe_fds[1]);  // 父进程不需要写端
        
        HLK_LOG_INFO("Parent process: waiting for child process (PID: %d)\n", child_pid);
        
        // 监听管道和子进程状态
        while (1) {
            current_time = zig_get_timestamp();
            
            // 检查是否超时
            if (current_time - start_time > OTA_DOWNLOAD_TIMEOUT) {
                HLK_LOG_ERR("OTA download timeout, killing child process\n");
                kill(child_pid, SIGTERM);
                sleep(2);
                kill(child_pid, SIGKILL);
                waitpid(child_pid, &status, 0);
                close(pipe_fds[0]);
                return CURLE_OPERATION_TIMEDOUT;
            }
            
            // 使用zig实现的select监听管道
            zig_FD_ZERO(&read_fds);
            zig_FD_SET(pipe_fds[0], &read_fds);
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            select_result = zig_select(pipe_fds[0] + 1, &read_fds, NULL, NULL, &timeout);
            
            if (select_result > 0 && zig_FD_ISSET(pipe_fds[0], &read_fds)) {
                // 有数据可读
                ssize_t bytes_read = read(pipe_fds[0], &result, sizeof(result));
                if (bytes_read > 0) {
                    HLK_LOG_INFO("Received from child: code=%d, progress=%d%%, msg=%s\n", 
                        result.result_code, result.download_progress, result.error_msg);
                    
                    // 如果是进度更新，向云端报告
                    if (result.result_code == CURLE_OK && result.download_progress > 0 && result.download_progress < 100) {
                        hlk_mqtt_report_version(HLK_OTA_NO_ERR, result.download_progress - 2, g_ota_msgid);
                    }
                }
            }
            
            // 检查子进程是否结束
            int wait_result = waitpid(child_pid, &status, WNOHANG);
            if (wait_result == child_pid) {
                // 子进程已结束
                HLK_LOG_INFO("Child process finished with status: %d\n", status);
                break;
            } else if (wait_result == -1) {
                HLK_LOG_ERR("waitpid failed: %s\n", strerror(errno));
                break;
            }
        }
        
        close(pipe_fds[0]);
        
        // 根据子进程退出状态返回结果
        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            HLK_LOG_INFO("Child process exited with code: %d\n", exit_code);
            return (exit_code == 0) ? CURLE_OK : CURLE_HTTP_RETURNED_ERROR;
        } else if (WIFSIGNALED(status)) {
            HLK_LOG_ERR("Child process killed by signal: %d\n", WTERMSIG(status));
            return CURLE_ABORTED_BY_CALLBACK;
        }
    }
    
    return CURLE_OK;
#endif
}

/******************************************************************************
 * 函数名    : hlk_mqtt_handle_ota
 * 功能描述  : 处理来自云端的OTA升级指令
 * 输入参数  : data - 接收到的OTA升级消息数据
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 解析云端下发的OTA升级指令，包括版本信息、下载地址、文件大小等
 *            启动固件下载和升级流程，并向云端反馈升级状态
 ******************************************************************************/
static void hlk_mqtt_handle_ota(MessageData *data) 
{
    char _host[32] = {0};      // 下载服务器主机名
    int _port = 0;             // 服务器端口号
    char _path[128] = {0};     // 下载文件路径
    VersionInfo get_;          // 版本信息结构体

    // 打印接收到的OTA消息（调试用）
        
    int len = data->message->payloadlen;
    char *str = data->message->payload;
    
    // 定义JSON解析相关变量
    cJSON *root = NULL, *version = NULL, *source = NULL;
    cJSON *msgid = NULL, *size = NULL, *FId = NULL;
    
    // 解析OTA升级JSON消息
    root = cJSON_Parse(str);
    if (root == NULL)
    {
        HLK_LOG_ERR("cJSON_Parse error\n");
        goto exit;
    }
    
    // 解析目标版本号
    version = cJSON_GetObjectItem(root, "Version");
    if (version == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem Version error\n");
        goto exit;
    }
    strcpy(get_.version, version->valuestring);

    // 解析固件下载地址
    source = cJSON_GetObjectItem(root, "Source");
    if (source == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem Source error\n");
        goto exit;
    }
    
    // 解析消息ID
    msgid = cJSON_GetObjectItem(root, "MsgId");
    if(msgid == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem MsgId error\n");
        goto exit;
    }
    g_ota_msgid = msgid->valueint;  // 保存到全局变量
    get_.msgid = msgid->valueint;   // 保存到版本信息结构体
    
    // 解析文件ID
    FId = cJSON_GetObjectItem(root, "FId");
    if(FId == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem FId error\n");
        goto exit;
    }
    g_ota_fid = FId->valueint;  // 保存文件ID到全局变量
    
    // 向云端确认收到升级指令
    hlk_mqtt_report_version(HLK_OTA_NO_ERR, 0, g_ota_msgid);
    
    // 解析文件大小
    size = cJSON_GetObjectItem(root, "FileSize");
    if (size == NULL)
    {
        HLK_LOG_ERR("cJSON_GetObjectItem FileSize error\n");
        goto exit;
    }
    g_ota_size = size->valueint;  // 保存文件大小到全局变量

    // 将版本信息写入本地文件，状态设为2（开始升级）
    //hlk_ota_write_version(SYSUPGRADE_MSGID_PATH, &get_, 2);
    
    // 解析下载URL，提取主机名、端口和路径
    HLK_LOG_INFO("source->valuestring:%s\n", source->valuestring);
    parse_http_url(source->valuestring, _host, &_port, _path);

    HLK_LOG_INFO("host:%s, port:%d, path:%s\n", _host, _port, _path);

    // 开始HTTP下载固件文件
    CURLcode res;
    res = hlk_ota_http(_host, _path);
    
    if (res == CURLE_OK) {
        HLK_LOG_INFO("Version upgrade running\n");
        
        // 下载成功，更新升级状态为1（准备安装）
        hlk_ota_write_version(SYSUPGRADE_MSGID_PATH, &get_, 1);
        
        // 执行固件烧录
        fota_Upgrade_Writing();
        
        // 等待5秒确保操作完成
        app_msleep(5000);
        
        // 重启设备以完成升级
        app_reboot();
    }else{
        // 下载失败，向云端报告错误
        hlk_mqtt_report_version(HLK_OTA_FLASH_CHECK_ERR, 0, g_ota_msgid);
        HLK_LOG_ERR("Version upgrade failed\n");
    }

exit:
    // 释放JSON对象内存
    if(root)
        cJSON_Delete(root);
}

extern void hlk_mqtt_handle_data_points_down(MessageData *pdata);   //在data_collector.c中定义

/******************************************************************************
 * 函数名    : mqtt_subscribe_parse
 * 功能描述  : 配置MQTT主题订阅和对应的消息处理函数
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 为各种MQTT主题绑定相应的消息处理回调函数
 *            包括APP控制、云端设置、设备重置、心跳回复、OTA升级等主题
 ******************************************************************************/
void mqtt_subscribe_parse()
{
    // 订阅APP控制主题，绑定APP消息处理函数
    MQTTSubscribe(hlk_iot.client, mqtt_topic_type_table[TOPIC_APP].topic, 0, hlk_mqtt_handle_app);
    
    // 订阅云端服务调用主题，绑定设置消息处理函数
    MQTTSubscribe(hlk_iot.client, mqtt_topic_type_table[TOPIC_GET].topic, 0, hlk_mqtt_handle_set);
    
    // 订阅设备重置回复主题，绑定重置消息处理函数
    MQTTSubscribe(hlk_iot.client, mqtt_topic_type_table[TOPIC_RESET_REPLY].topic, 0, hlk_mqtt_handle_reset);
    
    // 订阅心跳回复主题，绑定心跳回复处理函数
    MQTTSubscribe(hlk_iot.client, mqtt_topic_type_table[TOPIC_POST_PING_REPLY].topic, 0, hlk_mqtt_handle_ping_reply);
    
    // 订阅属性上报回复主题，绑定通用消息处理函数
    MQTTSubscribe(hlk_iot.client, mqtt_topic_type_table[TOPIC_POST_REPLY].topic, 0, messageArrived);
    
    // 订阅OTA升级主题，绑定OTA升级处理函数
    MQTTSubscribe(hlk_iot.client, mqtt_topic_type_table[TOPIC_UPGRADE].topic, 0, hlk_mqtt_handle_ota);


    MQTTSubscribe(hlk_iot.client, mqtt_topic_type_table[DATA_POINTS_DOWN].topic, 0, hlk_mqtt_handle_data_points_down);

    return;
}

/******************************************************************************
 * 函数名    : md5_encode
 * 功能描述  : 生成MQTT连接认证所需的MD5加密信息
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 基于设备信息和时间戳生成MQTT连接的用户名、密码等认证信息
 *            使用MD5算法对设备密钥和时间戳进行加密处理
 ******************************************************************************/
static void md5_encode(void)
{
    char mac[64] = {0};        // 设备MAC地址
    char local_ip[64] = {0};   // 本地IP地址
    char temp_key[64] = {0};   // 临时密钥
    char key[16] = {0};        // MD5加密结果
    time_t timesnow;           // 当前时间戳
    char timestamp[32];        // 时间戳字符串

    // 获取当前时间戳 - 使用Zig实现避免ABI兼容性问题
    timesnow = zig_get_timestamp();
    HLK_LOG_INFO("timesnow:%lld\n", (long long)timesnow);
    sprintf(timestamp, "%lld", (long long)timesnow);

    // 获取WAN接口的IP地址和MAC地址
    getWanIpAddress(WAN_INTERFACE, local_ip);
    get_interface_mac(WAN_INTERFACE, mac);

    // 构建MQTT客户端ID
    // 格式：productKey|固定字符串|模块类型|设备名称|网络类型
    sprintf(mqtt_connect_cret.id, "%s|%s|%s|%s|%s", 
            mqtt_user_cert.productKey, "adsfrm65tasd", MODULE_TYPE,
            mqtt_user_cert.deviceName, NET_TYPE);
    HLK_LOG_INFO("id:%s\n", mqtt_connect_cret.id);
    
    // 构建MQTT用户名
    // 格式：设备名称|时间戳000|本地IP|MAC地址
    sprintf(mqtt_connect_cret.user_name, "%s|%s000|%s|%s", 
            mqtt_user_cert.deviceName, timestamp, local_ip, mac);
    HLK_LOG_INFO("user_name = %s\n", mqtt_connect_cret.user_name);
    
    // 初始化密码前缀
    sprintf(mqtt_connect_cret.password, "$md5$%s$", timestamp);
    HLK_LOG_INFO("password:%s\n", mqtt_connect_cret.password);

    // 第一次MD5加密：对设备密钥进行加密
    MD5_CTX md5;
    memset(&md5, 0, sizeof(MD5_CTX));
    MD5Init(&md5);
    MD5Update(&md5, (unsigned char*)mqtt_user_cert.deviceSecret, strlen(mqtt_user_cert.deviceSecret));
    MD5Final(&md5, (unsigned char*)key);

    // 将MD5结果转换为十六进制字符串
    conver_hex2str((char *)key, sizeof(key), (char *)temp_key, sizeof(temp_key));

    // 将时间戳追加到第一次加密结果后
    strcat((char *)temp_key, (char *)timestamp);

    // 第二次MD5加密：对"第一次加密结果+时间戳"进行加密
    MD5Init(&md5);
    MD5Update(&md5, (unsigned char*)temp_key, strlen((char *)temp_key));
    MD5Final(&md5, (unsigned char*)key);

    // 清空临时缓冲区，准备存储最终结果
    memset(temp_key, 0, sizeof(temp_key));
    conver_hex2str((char *)key, sizeof(key), (char *)temp_key, sizeof(temp_key));

    // 将最终的MD5结果追加到密码字符串中
    strcat(mqtt_connect_cret.password, (char *)temp_key);

    return;
}

/******************************************************************************
 * 函数名    : hlk_MQTTYield
 * 功能描述  : MQTT消息循环处理和心跳维护主函数
 * 输入参数  : sharedData - 共享数据结构指针（当前未使用）
 * 输出参数  : 无
 * 返回值    : 0-成功 非0-失败
 * 说明      : MQTT连接的主循环函数，负责消息接收处理、心跳发送、连接维护等
 *            在连接建立后持续运行，直到连接断开或出现错误
 ******************************************************************************/
int hlk_MQTTYield(SHARED_DATA_S *sharedData)
{
    HLK_LOG_INFO("MQTTYield\r\n");
    int rc;
    
    time_t time_start, time_ping;
    time_start = zig_get_timestamp();  // 记录开始时间，用于心跳包定时

    // 初始化操作：订阅主题、发送首次心跳、检查OTA状态
    mqtt_subscribe_parse();   // 订阅所有需要的MQTT主题
    hlk_mqtt_ping();         // 发送第一个心跳包
    hlk_ota_check_version(); // 检查是否有待处理的OTA升级
    
    // MQTT消息处理主循环
    while (1)
    {
        // 处理MQTT消息，超时时间1000ms
        if ((rc = MQTTYield(hlk_iot.client, 1000)) != SUCCESS)
        {
            // MQTT连接出现问题，进行清理和重连准备
            HLK_LOG_ERR("rc = %d Disconnect_\r\n", rc);
            MQTTDisconnect(hlk_iot.client);           // 断开MQTT连接
            NetworkDisconnect(hlk_iot.network);        // 断开网络连接
            app_msleep(10 * 1000);                     // 等待10秒后重试
            sharedData->connect_status = MQTT_CONNECT_STATUS_DISCONNECTED;            // 连接状态设为0
            break;  // 跳出循环，返回上层进行重连
        }
        
        // 检查是否需要发送心跳包（每50秒发送一次）
        time_ping = zig_get_timestamp();
        if ((int)zig_time_diff_abs(time_start, time_ping) >= 50)
        {
            time_start = time_ping;  // 更新心跳时间基准
            hlk_mqtt_ping();         // 发送心跳包
        }

        // 主循环休眠1秒，避免CPU占用过高
        app_msleep(1000);
    }

    return rc;  // 返回错误码给上层处理
}

/******************************************************************************
 * 函数名    : hlk_mqtt_main
 * 功能描述  : MQTT功能初始化和配置主函数
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 0-成功 -1-失败
 * 说明      : MQTT模块的入口函数，根据连接类型进行相应的初始化操作
 *            支持海凌科私有云平台和其他MQTT平台的连接配置
 ******************************************************************************/
int hlk_mqtt_main()
{
    int ret = 0;
    
    // 设置MQTT用户认证信息指针
    hlk_iot.pmqtt_user_cert = &mqtt_user_cert;
    
    // 根据连接类型选择不同的MQTT服务器连接方式
    switch(hlk_iot.connnect_type)
    {
        // 连接海凌科私有云MQTT平台
        case MQTT_CONNECT_HLK:
            HLK_LOG_INFO("MQTT_CONNECT_HLK\r\n");
            
            // 获取设备五元组认证信息
            if(get_mqtt_user_certification_h() != 0){
                ret = -1;  // 获取认证信息失败
                break;
            } 
            
            // 生成MD5加密的连接认证信息
            HLK_LOG_INFO("md5_encode\r\n");
            md5_encode();
            
            // 初始化所有MQTT主题
            HLK_LOG_INFO("mqtt_topic_type_init\r\n"); 
            mqtt_topic_type_init();
            
            // 设置消息处理函数
            HLK_LOG_INFO("hlk_MQTTYield\r\n");
            hlk_iot.func = hlk_MQTTYield;
            break;
            
        // 连接其他第三方MQTT平台（预留接口）
        case MQTT_CONNECT_OTHER:
            HLK_LOG_INFO("MQTT_CONNECT_OTHER\r\n");
            // 注释掉的代码为其他平台连接预留
            //get_mqtt_user_certification_o();  // 获取其他平台认证
            //hlk_iot.func = other_MQTTYield;   // 设置其他平台处理函数
            break;
            
        // 默认连接方式（当前未实现）
        default:
            HLK_LOG_INFO("MQTT_CONNECT_DEFAULT\r\n");
            // 注释掉的代码为默认连接方式预留
            //get_mqtt_user_certification_h();
            //md5_encode();
            //mqtt_topic_type_init();
            break;
    }

    // 注释掉的代码为直接调用方式预留
    //mqtt_subscribe_parse(client);
    //hlk_mqtt_ping(client);
    
    return ret;  // 返回初始化结果
}

// 新增：数据推送到SSE的函数
static int push_data_to_sse(const char *channel, const char *data)
{
    int pipe_fd;
    char pipe_message[4096];
    int result = 0;
    
    if (!channel || !data) {
        HLK_LOG_ERR("push_data_to_sse: invalid parameters\n");
        return -1;
    }
    
    // 构建管道消息格式：通道名|数据
    snprintf(pipe_message, sizeof(pipe_message), "%s|%s\n", channel, data);
    
    // 打开命名管道（非阻塞模式）
    pipe_fd = open(SSE_DATA_PIPE_PATH, O_WRONLY | O_NONBLOCK);
    if (pipe_fd < 0) {
        // 如果管道不存在或无法打开，创建管道
        if (mkfifo(SSE_DATA_PIPE_PATH, 0666) == 0) {
            HLK_LOG_INFO("Created SSE data pipe: %s\n", SSE_DATA_PIPE_PATH);
            pipe_fd = open(SSE_DATA_PIPE_PATH, O_WRONLY | O_NONBLOCK);
        }
        
        if (pipe_fd < 0) {
            HLK_LOG_ERR("Failed to open SSE data pipe: %s\n", strerror(errno));
            return -1;
        }
    }
    
    // 写入数据到管道
    ssize_t written = write(pipe_fd, pipe_message, strlen(pipe_message));
    if (written < 0) {
        if (errno != EAGAIN && errno != EPIPE) {
            HLK_LOG_ERR("Failed to write to SSE pipe: %s\n", strerror(errno));
            result = -1;
        } else {
            HLK_LOG_ERR("No SSE clients listening (pipe full or broken)\n");
        }
    } else {
        HLK_LOG_INFO("Pushed data to SSE: channel=%s, size=%zd bytes\n", channel, written);
    }
    
    close(pipe_fd);
    return result;
}

/******************************************************************************
 * 函数名    : hlk_get_signature
 * 功能描述  : 生成HLK平台的API签名
 * 输入参数  : timestamp - 时间戳字符串
 *            Token     - 令牌字符串
 *            Nonce     - 随机数字符串
 * 输出参数  : signature - 生成的签名字符串
 * 返回值    : 签名字符串指针
 * 说明      : 根据小系统版本的签名算法，对参数值进行排序后直接拼接计算SHA1
 ******************************************************************************/
char *hlk_get_signature(char *timestamp, char *Token, char *Nonce, char *signature)
{
    // 打印传入参数
    HLK_LOG_INFO("Timestamp: %s\n", timestamp);
    HLK_LOG_INFO("Token: %s\n", Token);
    HLK_LOG_INFO("Nonce: %s\n", Nonce);

    char *a[3];
    a[0] = timestamp;
    a[1] = Token;
    a[2] = Nonce;
    char *tmp = NULL;

    // 排序参数值
    (memcmp(a[0], a[1], strlen(a[0])) < 0) ? (a[0] = a[0]) : (tmp = a[0], a[0] = a[1], a[1] = tmp);
    (memcmp(a[0], a[2], strlen(a[0])) < 0) ? (a[0] = a[0]) : (tmp = a[0], a[0] = a[2], a[2] = tmp);
    (memcmp(a[1], a[2], strlen(a[1])) < 0) ? (a[1] = a[1]) : (tmp = a[1], a[1] = a[2], a[2] = tmp);

    // 拼接字符串
    char sha1_ori[256] = {0};
    sprintf(sha1_ori, "%s%s%s", a[0], a[1], a[2]);
    HLK_LOG_INFO("strcat string:%s\n", sha1_ori);

    // 计算SHA1
    unsigned char sha1_result[SHA_DIGEST_LENGTH]; // SHA1结果是20字节
    SHA1((unsigned char *)sha1_ori, strlen(sha1_ori), sha1_result);

    // 将SHA1结果转换为十六进制字符串
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        sprintf(signature + (i * 2), "%02x", sha1_result[i]);
    }

    HLK_LOG_INFO("finish!!!!!  %s\r\n", signature);
    return signature;
}

/******************************************************************************
 * 函数名    : get_system_timestamp
 * 功能描述  : 获取系统时间戳，替代HLK的SNTP时间同步
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 时间戳（秒）
 * 说明      : 使用标准C库的time()函数获取当前时间戳
 *            如果时间看起来不合理（太小），则返回错误值
 ******************************************************************************/
time_t get_system_timestamp(void)
{
    time_t current_time = zig_get_timestamp();

    // 检查时间是否合理（大于2021-01-01 00:00:00 UTC的时间戳）
    if (current_time < 1609459200) {
        HLK_LOG_ERR("Warning: System time appears to be incorrect (timestamp: %lld)\n", current_time);
        HLK_LOG_ERR("Please ensure system time is properly synchronized\n");
        // 返回错误值，让调用者处理
        return 0;
    }

    return current_time;
}

/******************************************************************************
 * 函数名    : sync_system_time
 * 功能描述  : 同步系统时间
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 0-成功 -1-失败
 * 说明      : 尝试通过 NTP 或其他方式同步系统时间
 ******************************************************************************/
int sync_system_time(void)
{
    HLK_LOG_INFO("Attempting to sync system time...\n");

    // 方法1: 尝试使用 ntpdate 命令同步时间
    int ret = system("ntpdate -u pool.ntp.org > /dev/null 2>&1");
    if (ret == 0) {
        HLK_LOG_INFO("System time synchronized successfully using ntpdate\n");
        return 0;
    }

    HLK_LOG_ERR("Please ensure system time is manually synchronized\n");

    return -1;
}

size_t quest_write_callback(void *ptr, size_t size, size_t nmemb, void *stream)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)stream;

    HLK_LOG_INFO("%s\n", ptr);

    // 重新分配内存以容纳新数据
    char *ptr_realloc = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr_realloc == NULL) {
        HLK_LOG_ERR("quest_write_callback: realloc failed\n");
        return 0;
    }

    mem->memory = ptr_realloc;
    memcpy(&(mem->memory[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0; // 确保字符串以null结尾
    
    return realsize;
}

/******************************************************************************
 * 函数名    : query_request_address
 * 功能描述  : 查询MQTT服务器和WebAPI服务器地址
 * 输入参数  : 无
 * 输出参数  : 无
 * 返回值    : 0-失败 1-成功
 * 说明      : 使用libcurl发送HTTP POST请求到地址查询接口，解析返回的JSON数据
 ******************************************************************************/
 int query_request_address(void)
 {
     PRF("query_request_address\n");
     int ret = 0;
     CURL *curl = NULL;
     CURLcode res;
     struct MemoryStruct response_data = {0};
 
     // 初始化响应数据结构体
     response_data.memory = malloc(1024);
     if (response_data.memory == NULL) {
        PRF("Failed to allocate memory for response\n");
         return -1;
     }
     response_data.size = 0;
 
     // 获取设备信息
     ALINKDEV_t hlk_devinfo = {0};
     memset(&hlk_devinfo, 0, sizeof(ALINKDEV_t));
     ALINKDEV_t *g_hlk_devinfo = &hlk_devinfo;
 
     if (get_device_credentials(g_hlk_devinfo) != 0) {
        PRF("Failed to get device credentials\n");
         free(g_hlk_devinfo);
         free(response_data.memory);
         return -1;
     }

     PRF("DN:%s\n", g_hlk_devinfo->deviceName);
     PRF("PJ:%s\n", g_hlk_devinfo->projectKey);
     PRF("PK:%s\n", g_hlk_devinfo->productKey);
     PRF("PS:%s\n", g_hlk_devinfo->productSecret);
     PRF("DS:%s\n", g_hlk_devinfo->deviceSecret);
 
     // 获取系统时间戳，如果无效则尝试同步
     time_t time_now = get_system_timestamp();
     if (time_now == 0) {
        PRF("System time appears to be invalid, attempting to sync...\n");
 
         // 尝试同步系统时间
         if (sync_system_time() != 0) {
            PRF("Failed to sync system time\n");
             free(response_data.memory);
             return -1;
         }
 
         // 重新获取时间戳
         time_now = get_system_timestamp();
         if (time_now == 0) {
            PRF("System time is still invalid after sync attempt\n");
             free(response_data.memory);
             return -1;
         }
 
         PRF("System time synchronized successfully\n");
     }
 
     // 生成时间戳字符串
     char time_str[15] = {0};
     snprintf(time_str, sizeof(time_str), "%lld000", time_now);
 
     // 构建请求数据
     char data[100] = {0};
     char nonce[7] = {0};
     snprintf(nonce, 6, "%.5ld5", rand() % 9999);  // 生成随机nonce
     snprintf(data, sizeof(data), "Sn=%s&RType=%d&PType=%d", g_hlk_devinfo->deviceName, 3, 2);
 
     // 生成签名
     char signature[70] = {0};
     char *token = REQUEST_ADDRESS_TOKEN;
     hlk_get_signature(time_str, token, nonce, signature);
  

    /* 初始化libcurl全局环境 - 防止在无网络环境下出现段错误 */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        PRF("Failed to initialize libcurl global environment\n");
        free(response_data.memory);
        return -1;
    }


     // 初始化libcurl
     curl = curl_easy_init();
     if (!curl) {
        PRF("Failed to initialize curl\n");
         free(response_data.memory);
         return -1;
     }
 
     // 设置curl选项
     curl_easy_setopt(curl, CURLOPT_URL, REQUEST_ADDRESS);
     curl_easy_setopt(curl, CURLOPT_POST, 1L);
     curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
     curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(data));
 
     // 设置请求头
     struct curl_slist *headers = NULL;
     headers = curl_slist_append(headers, "Accept: application/json");
     headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
     char accessid_header[64];
     snprintf(accessid_header, sizeof(accessid_header), "AccessId: %s", REQUEST_ACCESSID);
     headers = curl_slist_append(headers, accessid_header);
     char nonce_header[32];
     snprintf(nonce_header, sizeof(nonce_header), "Nonce: %s", nonce);
     headers = curl_slist_append(headers, nonce_header);
     char signature_header[128];
     snprintf(signature_header, sizeof(signature_header), "Signature: %s", signature);
     headers = curl_slist_append(headers, signature_header);
    char timestamp_header[64];
    snprintf(timestamp_header, sizeof(timestamp_header), "Timestamp: %s", time_str);
    headers = curl_slist_append(headers, timestamp_header);
    char content_length_header[32];
    snprintf(content_length_header, sizeof(content_length_header), "Content-Length: %d", (int)strlen(data));
    headers = curl_slist_append(headers, content_length_header);
 
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
 
    // 设置响应数据回调
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, quest_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

    // 设置超时时间 - MT7688设备需要更严格的超时控制
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);  // 连接超时3秒

    // 添加错误缓冲区以获取详细错误信息
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    // 设置DNS缓存超时，避免DNS解析hang住
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 10L);

    // 禁用信号处理，避免在嵌入式环境下出现问题
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // 执行请求
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        PRF("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        if (strlen(errbuf) > 0) {
            PRF("Detailed error: %s\n", errbuf);
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        free(response_data.memory);
        return -1;
    }
 
    PRF("HTTP response: %s\n", response_data.memory);
 
     // 解析JSON响应
    cJSON *json = cJSON_Parse(response_data.memory);
    if (json == NULL) {
        PRF("Error parsing JSON response: %s\n", cJSON_GetErrorPtr());
         curl_easy_cleanup(curl);
         curl_slist_free_all(headers);
         free(response_data.memory);
        return -1;
    }
 
     // 检查响应状态码
     cJSON *code_status = cJSON_GetObjectItemCaseSensitive(json, "Code");
     if (!cJSON_IsNumber(code_status) || (code_status->valueint != 1)) {
        PRF("API request failed. Code: %d\n", code_status->valueint);
         cJSON_Delete(json);
         curl_easy_cleanup(curl);
         curl_slist_free_all(headers);
         free(response_data.memory);
         return -1;
     }
 
     // 获取Data字段
     cJSON *jsonData = cJSON_GetObjectItemCaseSensitive(json, "Data");
     if (jsonData != NULL && cJSON_IsArray(jsonData)) {
         int arraySize = cJSON_GetArraySize(jsonData);
         for (int i = 0; i < arraySize; i++) {
             cJSON *item = cJSON_GetArrayItem(jsonData, i);
             if (cJSON_IsObject(item)) {
                 cJSON *uType = cJSON_GetObjectItemCaseSensitive(item, "UType");
                 cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "Url");
 
                 if (cJSON_IsNumber(uType) && cJSON_IsString(url)) {
                     switch (uType->valueint) {
                     case 2: // MQTT服务器
                         strncpy(mqttUrl, url->valuestring, sizeof(mqttUrl) - 1);
                         mqttUrl[sizeof(mqttUrl) - 1] = '\0';
                         break;
                     case 3: // Web API服务器
                         strncpy(webApiUrl, url->valuestring, sizeof(webApiUrl) - 1);
                         webApiUrl[sizeof(webApiUrl) - 1] = '\0';
                         replace_https_with_http(webApiUrl);
                         break;
                     }
                 }
             }
         }
         ret = 1;
 
         PRF("MQTT Address: %s\n", mqttUrl);
         PRF("Web API Address: %s\n", webApiUrl);
 
         // 构建完整的API URL
         snprintf(postInfoUrl, sizeof(postInfoUrl), "%s%s", webApiUrl, POST_INIF_API);
         snprintf(postHistoryUrl, sizeof(postHistoryUrl), "%s%s", webApiUrl, POST_HISTORY_API);
         snprintf(postOtaInfoUrl, sizeof(postOtaInfoUrl), "%s%s", webApiUrl, POST_OTA_INFO);
 
         PRF("postInfoUrl: %s\n", postInfoUrl);
         PRF("postHistoryUrl: %s\n", postHistoryUrl);
         PRF("postOtaInfoUrl: %s\n", postOtaInfoUrl);
     } else {
        PRF("Data field is missing or not an array\n");
     }
 
     // 清理资源
     cJSON_Delete(json);
     curl_easy_cleanup(curl);
     curl_slist_free_all(headers);
     free(response_data.memory);
 
     PRF("query_request_address completed with ret = %d\n", ret);
     return ret;
 }

/******************************************************************************
 * 函数名    : switch_mqtt_url
 * 功能描述  : 切换MQTT服务器地址
 * 输入参数  : url - 指向URL字符串指针的指针
 * 输出参数  : 无
 * 返回值    : 无
 * 说明      : 查询MQTT服务器地址并更新全局变量
 ******************************************************************************/
void switch_mqtt_url(char **url)
{
    PRF("switch_mqtt_url\n");
    static char url_store[MQTT_URL_COUNT][MQTT_URL_LEN] = {0};
    int ret = -1;

    // 查询MQTT和WEB地址
    while (ret != 1)
    {
        ret = query_request_address();
        PRF("query_request_address ret = %d\n", ret);
        if (ret == 1)
        {
            // 查询成功，设置MQTT连接地址
            strncpy(url_store[0], mqttUrl, MQTT_URL_LEN-1);
            url_store[0][MQTT_URL_LEN-1] = '\0';

            // 添加备用地址
            strncpy(url_store[1], MQTT_URL, MQTT_URL_LEN-1);
            url_store[1][MQTT_URL_LEN-1] = '\0';
        }
        
        app_msleep(1000);
    }
    *url = url_store[0];
}


