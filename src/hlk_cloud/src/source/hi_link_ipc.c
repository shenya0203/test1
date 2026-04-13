/******************************************************************************
                        Copyright (C),  
 ******************************************************************************
  Filename   : hi_link_ipc.c
  Version    : v1.0
  Description: hi_link IPC服务实现，基于Unix域套接字和JSON
 *****************************************************************************/

/*****************************************************************************
 *                                INCLUDE                                     *
 *****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>

#include "hi_link_ipc.h"
#include "hi_link.h"
#include "hlk_log.h"
#include "hi_mqtt.h"
#include "cJSON.h"
#include "app_api.h"

/*****************************************************************************
 *                              LOCAL DEFINE                                  *
 *****************************************************************************/
#define IPC_DEBUG 1
#if IPC_DEBUG
#define IPC_LOG(fmt, args...)                                   \
    do                                                         \
    {                                                          \
        syslog(LOG_INFO, "[IPC] %s:%d: ", __FUNCTION__, __LINE__);       \
        syslog(LOG_INFO, fmt, ##args);                                   \
    } while (0)
#else
#define IPC_LOG(fmt, args...)
#endif

// 添加客户端管理全局变量
static ipc_client_info_t g_clients[MAX_CLIENTS];
static int g_client_count = 0;
static pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/*****************************************************************************
 *                             LOCAL VARIABLES                                *
 *****************************************************************************/
static pthread_t g_ipc_thread_id = 0;         /* IPC服务线程ID */
static int g_server_fd = -1;                  /* 服务器套接字文件描述符 */
static volatile hi_link_ipc_status_e g_ipc_status = IPC_SERVICE_STOPPED; /* 服务状态 */
static volatile int g_running = 0;            /* 运行标志 */
static pthread_mutex_t g_ipc_mutex = PTHREAD_MUTEX_INITIALIZER; /* 互斥锁 */

/* 新增：设备配置管理 */
static device_config_t g_device_config = {0};
static bool g_config_initialized = false;

/*****************************************************************************
 *                            FUNCTION DECLARATIONS                           *
 *****************************************************************************/
static void *hi_link_ipc_thread(void *arg);
static int process_client_message(int client_fd, char *buffer, size_t len);
static cJSON* handle_get_connection_status(cJSON *params);
static cJSON* handle_get_channel_data(cJSON *params);
static cJSON* handle_send_channel_data(cJSON *params);
static cJSON* handle_get_ota_status(cJSON *params);
static void send_json_response(int client_fd, cJSON *result, cJSON *error, int id);
static cJSON* handle_start_channel_listener(cJSON *params);

/*****************************************************************************
 *                              IMPLEMENTATION                                *
 *****************************************************************************/

/**
 * @brief 初始化默认设备配置
 */
static void init_default_device_config(void)
{
    if (g_config_initialized) {
        return;
    }
    
    /* 设置默认设备信息 */
    strncpy(g_device_config.device_id,  mqtt_user_cert.deviceName, MAX_DEVICE_ID_LEN - 1);
    strncpy(g_device_config.product_id, mqtt_user_cert.productKey, MAX_PRODUCT_ID_LEN - 1);

    IPC_LOG("设备ID: %s, 产品ID: %s\n", g_device_config.device_id, g_device_config.product_id);
    
    /* 添加默认通道配置 */
    g_device_config.channel_count = 0;
    
    /* SSID通道 */
    strncpy(g_device_config.channels[0].name, "TRANSDOWN", MAX_CHANNEL_NAME_LEN - 1);
    g_device_config.channels[0].type = CHANNEL_TYPE_STRING;
    g_device_config.channels[0].enabled = true;
    strncpy(g_device_config.channels[0].description, "透传下行", 127);
    g_device_config.channels[0].handler = NULL;
    g_device_config.channel_count++;
    
    /* WiFi密码通道 */
    strncpy(g_device_config.channels[1].name, "TRANSUP", MAX_CHANNEL_NAME_LEN - 1);
    g_device_config.channels[1].type = CHANNEL_TYPE_STRING;
    g_device_config.channels[1].enabled = true;
    strncpy(g_device_config.channels[1].description, "透传上行", 127);
    g_device_config.channels[1].handler = NULL; //透传上行默认处理函数
    g_device_config.channel_count++;
    
    g_config_initialized = true;
    IPC_LOG("Default device config initialized with %d channels\n", g_device_config.channel_count);
}

/**
 * @brief 初始化hi_link IPC服务
 * @return 0成功，非0失败
 */
int hi_link_ipc_init(void)
{
    struct sockaddr_un server_addr;
    int ret;
    
    /* 防止重复初始化 */
    if (g_ipc_status == IPC_SERVICE_RUNNING) {
        IPC_LOG("IPC服务已经在运行\n");
        return 0;
    }

    /* 创建Unix域套接字 */
    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        IPC_LOG("创建套接字失败: %s\n", strerror(errno));
        return -1;
    }

    /* 设置服务器地址 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, HI_LINK_SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    /* 删除可能存在的旧套接字文件 */
    unlink(HI_LINK_SOCKET_PATH);

    /* 绑定套接字 */
    ret = bind(g_server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        IPC_LOG("绑定套接字失败: %s\n", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    /* 设置套接字权限 */
    chmod(HI_LINK_SOCKET_PATH, 0666);

    /* 监听连接 */
    ret = listen(g_server_fd, MAX_CLIENTS);
    if (ret < 0) {
        IPC_LOG("监听套接字失败: %s\n", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        unlink(HI_LINK_SOCKET_PATH);
        return -1;
    }

    /* 初始化设备配置 */
    //init_default_device_config();

    /* 启动IPC服务线程 */
    g_running = 1;
    ret = pthread_create(&g_ipc_thread_id, NULL, hi_link_ipc_thread, NULL);
    if (ret != 0) {
        IPC_LOG("创建IPC线程失败: %s\n", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        unlink(HI_LINK_SOCKET_PATH);
        return -1;
    }

    g_ipc_status = IPC_SERVICE_RUNNING;
    IPC_LOG("IPC服务初始化成功，监听于 %s\n", HI_LINK_SOCKET_PATH);
    
    return 0;
}

/**
 * @brief 清理hi_link IPC服务
 */
void hi_link_ipc_cleanup(void)
{
    /* 防止重复清理 */
    if (g_ipc_status == IPC_SERVICE_STOPPED) {
        return;
    }

    /* 设置停止标志 */
    g_running = 0;

    /* 关闭服务器套接字，强制线程退出 */
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    /* 等待线程退出 */
    if (g_ipc_thread_id != 0) {
        pthread_join(g_ipc_thread_id, NULL);
        g_ipc_thread_id = 0;
    }

    /* 删除套接字文件 */
    unlink(HI_LINK_SOCKET_PATH);

    g_ipc_status = IPC_SERVICE_STOPPED;
    IPC_LOG("IPC服务已清理\n");
}

/**
 * @brief 获取IPC服务状态
 * @return IPC服务状态枚举值
 */
hi_link_ipc_status_e hi_link_ipc_get_status(void)
{
    return g_ipc_status;
}

/**
 * @brief 注册客户端监听的通道
 */
int hi_link_ipc_register_client_channel(int client_fd, const char *client_id, const char *channel_name)
{
    if (!client_id || !channel_name) {
        return -1;
    }
    
    pthread_mutex_lock(&g_clients_mutex);
    
    // 查找现有客户端或添加新客户端
    ipc_client_info_t *client = NULL;
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].fd == client_fd) {
            client = &g_clients[i];
            break;
        }
    }
    
    // 如果是新客户端，添加到列表
    if (!client && g_client_count < MAX_CLIENTS) {
        client = &g_clients[g_client_count++];
        client->fd = client_fd;
        strncpy(client->client_id, client_id, sizeof(client->client_id) - 1);
        client->channel_count = 0;
        client->connect_time = zig_get_timestamp();
        client->active = true;
    }
    
    if (client && client->channel_count < MAX_CHANNELS) {
        // 检查是否已经监听该通道
        bool already_listening = false;
        for (int i = 0; i < client->channel_count; i++) {
            if (strcmp(client->listening_channels[i], channel_name) == 0) {
                already_listening = true;
                break;
            }
        }
        
        if (!already_listening) {
            strncpy(client->listening_channels[client->channel_count], 
                   channel_name, MAX_CHANNEL_NAME_LEN - 1);
            client->channel_count++;
            IPC_LOG("Client %s now listening to channel: %s\n", client_id, channel_name);
        }
    }
    
    pthread_mutex_unlock(&g_clients_mutex);
    return 0;
}

int hi_link_ipc_unregister_client_channel(int client_fd)
{
    if (client_fd <= 0) {
        return -1;
    }

    pthread_mutex_lock(&g_clients_mutex);

    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].fd == client_fd) {
            g_clients[i].active = false;
            g_clients[i].fd = -1;
            g_clients[i].channel_count = 0;
            g_clients[i].connect_time = 0;
            g_clients[i].client_id[0] = '\0';
            for (int j = 0; j < MAX_CHANNELS; j++) {
                g_clients[i].listening_channels[j][0] = '\0';
            }
            break;
        }
    }

    pthread_mutex_unlock(&g_clients_mutex);
    return 0;
}

/**
 * @brief IPC服务线程
 * @param arg 线程参数
 * @return 线程返回值
 */
static void *hi_link_ipc_thread(void *arg)
{
    fd_set read_fds;
    int client_fds[MAX_CLIENTS] = {-1};
    int max_fd, i, client_count = 0;
    struct timeval tv;
    char buffer[MAX_BUF_SIZE];
    int ret;
    /* 初始化客户端文件描述符数组 */
    for (i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
    }

    /* 忽略SIGPIPE信号 */
    signal(SIGPIPE, SIG_IGN);

    /* 主循环 */
    while (g_running) {
        /* 设置select参数 */
        FD_ZERO(&read_fds);
        FD_SET(g_server_fd, &read_fds);
        max_fd = g_server_fd;

        /* 添加现有客户端到select集合 */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] > 0) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];
                }
            }
        }

        /* 设置超时 */
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        /* 等待事件 */
        ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;  /* 被信号中断，继续 */
            }
            IPC_LOG("select失败: %s\n", strerror(errno));
            break;
        } else if (ret == 0) {
            continue;  /* 超时，继续 */
        }

        /* 检查新的连接 */
        if (FD_ISSET(g_server_fd, &read_fds)) {
            int client_fd = accept(g_server_fd, NULL, NULL);
            if (client_fd >= 0) {
                /* 查找空闲的客户端槽位 */
                int slot = -1;
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] == -1) {
                        slot = i;
                        break;
                    }
                }

                if (slot != -1) {
                    /* 将客户端添加到监听列表 */
                    client_fds[slot] = client_fd;
                    client_count++;
                    IPC_LOG("新客户端连接，fd=%d\n", client_fd);
                } else {
                    /* 没有空闲槽位，关闭连接 */
                    IPC_LOG("达到最大客户端数量限制，拒绝连接\n");
                    close(client_fd);
                }
            }
        }

        /* 处理客户端消息 */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] != -1 && FD_ISSET(client_fds[i], &read_fds)) {
                /* 读取客户端消息 */
                memset(buffer, 0, sizeof(buffer));
                ret = read(client_fds[i], buffer, sizeof(buffer) - 1);

                if (ret > 0) {
                    IPC_LOG("收到客户端消息长度: %d\n", ret);
                    /* 处理消息 */
                    process_client_message(client_fds[i], buffer, ret);
                } else if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    /* 客户端断开连接或错误 */
                    IPC_LOG("客户端断开连接，fd=%d\n", client_fds[i]);
                    hi_link_ipc_unregister_client_channel(client_fds[i]);

                    close(client_fds[i]);
                    client_fds[i] = -1;
                    client_count--;
                    // 删除客户端注册的通道
                }
            }
        }
    }

    /* 关闭所有客户端连接 */
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] != -1) {
            close(client_fds[i]);
        }
    }

    IPC_LOG("IPC服务线程退出\n");
    return NULL;
}

/**
 * @brief 获取通道列表处理函数
 * @param params 参数JSON对象
 * @return 结果JSON对象，调用者负责释放
 */
cJSON* handle_get_channel_list(cJSON *params)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *channels_array = cJSON_CreateArray();
    
    if (!g_config_initialized) {
        init_default_device_config();
    }

    pthread_mutex_lock(&g_ipc_mutex);
    
    /* 遍历所有通道配置 */
    for (int i = 0; i < g_device_config.channel_count; i++) {
        channel_config_t *ch = &g_device_config.channels[i];
        
        cJSON *channel_info = cJSON_CreateObject();
        cJSON_AddStringToObject(channel_info, "name", ch->name);
        cJSON_AddStringToObject(channel_info, "type", 
                               (ch->type == CHANNEL_TYPE_STRING) ? "string" :
                               (ch->type == CHANNEL_TYPE_NUMBER) ? "number" : "boolean");
        cJSON_AddBoolToObject(channel_info, "enabled", ch->enabled);
        cJSON_AddStringToObject(channel_info, "description", ch->description);
        
        cJSON_AddItemToArray(channels_array, channel_info);
    }
    
    pthread_mutex_unlock(&g_ipc_mutex);

    /* 构建返回结果 */
    cJSON_AddBoolToObject(result, "success", 1);
    cJSON_AddStringToObject(result, "device_id", g_device_config.device_id);
    cJSON_AddStringToObject(result, "product_id", g_device_config.product_id);
    cJSON_AddNumberToObject(result, "channel_count", g_device_config.channel_count);
    cJSON_AddItemToObject(result, "channels", channels_array);

    //在返回结果中添加固件版本
    char version[128] = {0};
    get_version_info(version);
    cJSON_AddStringToObject(result, "firmware_version", version);
    
    return result;
}

// 新增：处理客户端注册监听请求的函数
static cJSON* handle_register_listener(int client_fd, cJSON *params)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *client_id_obj = NULL;
    cJSON *channels_obj = NULL;
    const char *client_id = NULL;
    int registered_count = 0;

    // 获取客户端ID
    client_id_obj = cJSON_GetObjectItem(params, "client_id");
    if (!client_id_obj || !cJSON_IsString(client_id_obj)) {
        cJSON_AddBoolToObject(result, "success", false);
        cJSON_AddStringToObject(result, "error", "缺少client_id参数或格式错误");
        IPC_LOG("缺少client_id参数或格式错误\n");
        return result;
    }
    client_id = client_id_obj->valuestring;

    // 获取要监听的通道列表
    channels_obj = cJSON_GetObjectItem(params, "channels");
    if (!channels_obj || !cJSON_IsArray(channels_obj)) {
        cJSON_AddBoolToObject(result, "success", false);
        cJSON_AddStringToObject(result, "error", "缺少channels参数或格式错误");
        IPC_LOG("缺少channels参数或格式错误\n");
        return result;
    }

    // 注册监听每个通道
    int channel_count = cJSON_GetArraySize(channels_obj);
    for (int i = 0; i < channel_count; i++) {
        cJSON *channel_item = cJSON_GetArrayItem(channels_obj, i);
        if (cJSON_IsString(channel_item)) {
            const char *channel_name = channel_item->valuestring;
            if (hi_link_ipc_register_client_channel(client_fd, client_id, channel_name) == 0) {
                registered_count++;
                IPC_LOG("注册客户端 %s 监听通道: %s\n", client_id, channel_name);
            }
        }
    }

    // 构建返回结果
    cJSON_AddBoolToObject(result, "success", registered_count > 0);
    cJSON_AddStringToObject(result, "client_id", client_id);
    cJSON_AddNumberToObject(result, "registered_channels", registered_count);
    cJSON_AddNumberToObject(result, "timestamp", (double)zig_get_timestamp());

    if (registered_count == 0) {
        cJSON_AddStringToObject(result, "error", "没有成功注册任何通道");
        IPC_LOG("没有成功注册任何通道\n");
    }

    return result;
}

/**
 * @brief 处理客户端消息
 * @param client_fd 客户端套接字
 * @param buffer 消息缓冲区
 * @param len 消息长度
 * @return 0成功，非0失败
 */
static int process_client_message(int client_fd, char *buffer, size_t len)
{
    cJSON *root = NULL;
    cJSON *method = NULL;
    cJSON *params = NULL;
    cJSON *id_obj = NULL;
    cJSON *result = NULL;
    cJSON *error = NULL;
    int id = 0;
    int ret = 0;

    /* 解析JSON消息 */
    root = cJSON_Parse(buffer);
    if (!root) {
        IPC_LOG("解析JSON消息失败\n");
        error = cJSON_CreateString("无效的JSON格式");
        send_json_response(client_fd, NULL, error, 0);
        cJSON_Delete(error);
        return -1;
    }
    IPC_LOG("解析JSON消息成功\n");

    /* 获取method字段 */
    method = cJSON_GetObjectItem(root, "method");
    if (!method || !cJSON_IsString(method)) {
        IPC_LOG("缺少method字段或格式错误\n");
        error = cJSON_CreateString("缺少method字段或格式错误");
        send_json_response(client_fd, NULL, error, 0);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return -1;
    }
    IPC_LOG("method: %s\n", method->valuestring);

    /* 获取params字段 */
    params = cJSON_GetObjectItem(root, "params");
    if (!params) {
        params = cJSON_CreateObject();
    }


    /* 根据method处理请求 */
    if (strcmp(method->valuestring, "get_connection_status") == 0) {
        result = handle_get_connection_status(params);
    } else if (strcmp(method->valuestring, "get_channel_data") == 0) {
        result = handle_get_channel_data(params);
    } else if (strcmp(method->valuestring, "send_channel_data") == 0) {
        IPC_LOG("send_channel_data\n");
        result = handle_send_channel_data(params);
    } else if (strcmp(method->valuestring, "get_ota_status") == 0) {
        result = handle_get_ota_status(params);
    } else if (strcmp(method->valuestring, "get_channel_list") == 0) {
        result = handle_get_channel_list(params);
    } else if (strcmp(method->valuestring, "start_channel_listener") == 0) {
        result = handle_start_channel_listener(params);
    
    } else if (strcmp(method->valuestring, "register_listener") == 0) { // 新增：处理客户端注册监听通道的请求
        IPC_LOG("register_listener\n");
        result = handle_register_listener(client_fd, params);
    } else {
        IPC_LOG("未知的方法: %s\n", method->valuestring);
        error = cJSON_CreateString("未知的方法");
        ret = -1;
    }

    /* 发送响应 */
    send_json_response(client_fd, result, error, id);

    /* 清理 */
    if (error) cJSON_Delete(error);
    cJSON_Delete(root);

    return ret;
}

/**
 * @brief 获取连接状态处理函数
 * @param params 参数JSON对象
 * @return 结果JSON对象，调用者负责释放
 */
static cJSON* handle_get_connection_status(cJSON *params)
{
    cJSON *result = cJSON_CreateObject();
    int status = 0;  /* 默认未连接 */

    /* 这里获取实际的MQTT连接状态 */
    /* 在此假设有外部函数或变量来获取连接状态 */
    pthread_mutex_lock(&g_ipc_mutex);
    /* 这里应该调用实际获取MQTT连接状态的函数 */
    /* 例如： status = get_mqtt_connection_status(); */
    
    /* 临时模拟，实际应替换为真实实现 */
    status = sharedData.connect_status;
    pthread_mutex_unlock(&g_ipc_mutex);

    /* 构建返回结果 */
    cJSON_AddNumberToObject(result, "status", status);
    cJSON_AddStringToObject(result, "status_desc", 
                            (status == 0) ? "Not Connected" : 
                            (status == 1) ? "Connecting" : 
                            (status == 2) ? "Connected" : 
                            (status == 3) ? "Connect failed" : "Unknown status");
    
    return result;
}

/**
 * @brief 获取MQTT主题数据处理函数
 * @param params 参数JSON对象
 * @return 结果JSON对象，调用者负责释放
 */
static cJSON* handle_get_channel_data(cJSON *params)
{
    cJSON *topic_obj;
    cJSON *result = cJSON_CreateObject();
    char *topic = NULL;
    char data_buffer[MAX_BUF_SIZE] = {0};
    int success = 0;

    /* 获取MQTT主题参数 */
    topic_obj = cJSON_GetObjectItem(params, "topic");
    if (!topic_obj || !cJSON_IsString(topic_obj)) {
        cJSON_AddBoolToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", "缺少topic参数或格式错误");
        return result;
    }
    
    topic = topic_obj->valuestring;
    IPC_LOG("获取通道[%s]数据\n", topic);
    //本地数据获取 需要

    pthread_mutex_lock(&g_ipc_mutex);
    /* 这里应该获取指定MQTT主题的最新数据 */
    /* 从MQTT客户端的缓存或回调数据中获取 */
    
    /* 遍历主题表查找匹配的主题 */
    for (int i = MQTT_TOPIC_TYPE_START + 1; i < MQTT_TOPIC_TYPE_END; i++) {
        if (strcmp(mqtt_topic_type_table[i].topic, topic) == 0) {
            /* 获取该主题的最新消息 */
            /* 这里需要实际实现从MQTT客户端获取数据的逻辑 */
            success = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_ipc_mutex);

    /* 构建返回结果 */
    cJSON_AddBoolToObject(result, "success", success);
    if (success) {
        cJSON_AddStringToObject(result, "topic", topic);
        cJSON_AddStringToObject(result, "data", data_buffer);
        cJSON_AddNumberToObject(result, "timestamp", (double)time(NULL));
    } else {
        cJSON_AddStringToObject(result, "error", "获取主题数据失败或主题不存在");
    }
    
    return result;
}


/**
 * @brief 查找通道配置
 * @param channel_name 通道名称
 * @return 通道配置指针，未找到返回NULL
 */
static channel_config_t* find_channel_config(const char *channel_name)
{
    if (!channel_name || !g_config_initialized) {
        return NULL;
    }
    
    for (int i = 0; i < g_device_config.channel_count; i++) {
        if (strcmp(g_device_config.channels[i].name, channel_name) == 0 &&
            g_device_config.channels[i].enabled) {
            return &g_device_config.channels[i];
        }
    }
    
    return NULL;
}

/**
 * @brief 构建云端标准主题
 * @param topic_buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @param suffix 主题后缀 (如: "thing/property/set")
 * @return 0成功，非0失败
 */
static int build_cloud_topic(char *topic_buffer, size_t buffer_size, const char *suffix)
{
    if (!topic_buffer || !suffix || !g_config_initialized) {
        return -1;
    }
    
    int ret = snprintf(topic_buffer, buffer_size, "sys/%s/%s/%s", 
                       g_device_config.product_id, 
                       g_device_config.device_id, 
                       suffix);
    
    return (ret >= buffer_size) ? -1 : 0;
}

static char *build_cloud_data(const char *channel_name, const cJSON *channel_value)
{
    char *data = NULL;
    int ret = 0;
    cJSON *root = cJSON_CreateObject();
    cJSON *input_data = cJSON_CreateObject();
    time_t now = zig_get_timestamp();
    char *response_str = NULL;
    
    if (!root || !input_data) {
        if (root) cJSON_Delete(root);
        if (input_data) cJSON_Delete(input_data);
        return NULL;
    }

    //数据示例：{"DeviceCode":"aU1FFk0mGWP","Items":[{"Time":1751955934,"Name":"Powerstate","Value":false}]}
    
    cJSON_AddStringToObject(input_data, "Name", channel_name);
    if (cJSON_IsString(channel_value)) {
        cJSON_AddStringToObject(input_data, "Value", channel_value->valuestring);
    } else if (cJSON_IsNumber(channel_value)) {
        cJSON_AddNumberToObject(input_data, "Value", channel_value->valueint);
    } else if (cJSON_IsBool(channel_value)) {
        cJSON_AddBoolToObject(input_data, "Value", channel_value->valueint);
    } else if (cJSON_IsNull(channel_value)) {
        cJSON_AddNullToObject(input_data, "Value");
    } else if (cJSON_IsArray(channel_value)) {
        cJSON_AddItemToObject(input_data, "Value", channel_value);
    } else if (cJSON_IsObject(channel_value)) {
        cJSON_AddItemToObject(input_data, "Value", channel_value);
    } else {
        cJSON_AddStringToObject(input_data, "Value", channel_value->valuestring);
    }
    
    cJSON_AddNumberToObject(input_data, "Time", now);
    cJSON *array = cJSON_CreateArray();

    cJSON_AddStringToObject(root, "DeviceCode", g_device_config.device_id);
    cJSON_AddItemToArray(array, input_data);
    cJSON_AddItemToObject(root, "Items", array);

    response_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    return response_str;
}

/**
 * @brief 修改后的发送MQTT主题数据处理函数
 * @param params 参数JSON对象
 * @return 结果JSON对象，调用者负责释放
 */
static cJSON* handle_send_channel_data(cJSON *params)
{
    cJSON *channel_obj, *data_obj;
    cJSON *result = cJSON_CreateObject();
    cJSON *Name = NULL;
    cJSON *Value = NULL;
    char *data = NULL;
    char *parsed_channel_name = NULL;
    cJSON *parsed_channel_value = NULL;
    channel_config_t *channel_config = NULL;
    char topic_buffer[256];
    char *response_data = NULL;
    int success = 0;

    if (!g_config_initialized) {
        init_default_device_config();
    }

    char *params_str = cJSON_PrintUnformatted(params);
    IPC_LOG("params: %s\n", params_str);
    free(params_str);
    
    //接收数据示例：
    //{"method":"send_channel_data","params":{"channel":"TRANSUP","data":"{\"Name\":\"TRANSUP\",\"Value\":111111111111111}"},"id":3}

    /* 获取通道名称参数 */
    channel_obj = cJSON_GetObjectItem(params, "channel");
    if (!channel_obj || !cJSON_IsString(channel_obj)) {
        cJSON_AddBoolToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", "缺少channel参数或格式错误");
        return result;
    }
    IPC_LOG("channel_obj: %s\n", channel_obj->valuestring);
    
    /* 获取数据参数 */
    data_obj = cJSON_GetObjectItem(params, "data");
    if (!data_obj || !cJSON_IsObject(data_obj)) {
        cJSON_AddBoolToObject(result, "success", 0);
        cJSON_AddStringToObject(result, "error", "缺少data参数或格式错误");
        IPC_LOG("data is not object");
        return result;
    }
        
    Name = cJSON_GetObjectItem(data_obj, "Name");
    Value = cJSON_GetObjectItem(data_obj, "Value");

    IPC_LOG("Channel_name: %s\n", Name->valuestring);

    parsed_channel_name = Name->valuestring;

    pthread_mutex_lock(&g_ipc_mutex);
    /* 查找通道配置 */
    channel_config = find_channel_config(parsed_channel_name);
    if (!channel_config) {
        IPC_LOG("Channel not found or disabled: %s\n", parsed_channel_name);
        cJSON_AddStringToObject(result, "error", "通道不存在或已禁用");
        pthread_mutex_unlock(&g_ipc_mutex);
        goto cleanup;
    }
    success = 1;

    /* 调用通道处理函数 */
    if (channel_config->handler != NULL) {
        IPC_LOG("Calling handler for channel: %s\n", parsed_channel_name);
        channel_config->handler(parsed_channel_name, Value);
    } else {
        IPC_LOG("No handler found for channel: %s\n", parsed_channel_name);
        cJSON_AddStringToObject(result, "error", "通道处理函数未定义");
    }

    /* 构建响应主题并发送响应 */
    if (success) {
        //构建响应数据
        int ret = 0;
        char *cloud_data = build_cloud_data(parsed_channel_name, Value);
        if (cloud_data) {
            ret = hlk_mqtt_publish(mqtt_topic_type_table[TOPIC_POST].topic, QOS0, cloud_data, strlen(cloud_data));
            if (ret != 0) {
                IPC_LOG("Failed to publish response: %d\n", ret);
                success = 0;
            }
            free(cloud_data);
        }
    }
    
    pthread_mutex_unlock(&g_ipc_mutex);

cleanup:
    /* 构建返回结果 */
    cJSON_AddBoolToObject(result, "success", success);
    if (success) {
        cJSON_AddStringToObject(result, "channel", parsed_channel_name);
        cJSON_AddStringToObject(result, "processed_by", channel_config ? channel_config->description : "Unknown");
        cJSON_AddNumberToObject(result, "timestamp", (double)time(NULL));
    } else {
        cJSON_AddStringToObject(result, "error", "处理通道数据失败");
    }
    
    return result;
}

/**
 * @brief 获取OTA状态处理函数
 * @param params 参数JSON对象
 * @return 结果JSON对象，调用者负责释放
 */
static cJSON* handle_get_ota_status(cJSON *params)
{
    cJSON *result = cJSON_CreateObject();
    VersionInfo versionInfo; /* 假设这是定义在hi_mqtt.c中的OTA状态变量 */

    if (hlk_ota_read_version(SYSUPGRADE_MSGID_PATH, &versionInfo) < 0){
        // 读取文件失败或文件不存在，说明没有待处理的升级
        HLK_LOG_ERR("Read %s err\r\n", SYSUPGRADE_MSGID_PATH);
    }

    pthread_mutex_lock(&g_ipc_mutex);
    /* 获取实际的OTA状态 */
    /* 这里应该使用实际的OTA状态变量 */
    
    /* 构建返回结果 */
    cJSON_AddNumberToObject(result, "progress", versionInfo.progress);  // 升级流程 0未开始 1已完成 2升级中
    cJSON_AddNumberToObject(result, "status", versionInfo.progress > 0 ? 2 : 0); /* 0-无升级, 2-升级中 */
    cJSON_AddStringToObject(result, "version", versionInfo.version);
    cJSON_AddStringToObject(result, "error_msg", "");
    pthread_mutex_unlock(&g_ipc_mutex);
    
    return result;
}

/**
 * @brief 处理开始通道监听请求
 * @param params JSON参数对象
 * @return JSON结果对象
 */
static cJSON* handle_start_channel_listener(cJSON *params)
{
    cJSON *result = NULL;
    cJSON *channel_obj = NULL;
    const char *channel_name = NULL;
    
    if (!params) {
        IPC_LOG("handle_start_channel_listener: 参数为空\n");
        return cJSON_CreateNull();
    }
    
    // 获取通道名称
    channel_obj = cJSON_GetObjectItem(params, "channel");
    if (!channel_obj || !cJSON_IsString(channel_obj)) {
        IPC_LOG("handle_start_channel_listener: 缺少或无效的channel参数\n");
        return cJSON_CreateNull();
    }
    
    channel_name = channel_obj->valuestring;
    IPC_LOG("开始监听通道: %s\n", channel_name);
    
    // 创建成功响应
    result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", cJSON_True);
    cJSON_AddStringToObject(result, "message", "通道监听已启动");
    cJSON_AddStringToObject(result, "channel", channel_name);
    
    // 这里应该启动对指定通道的监听机制
    // 可以通过注册回调函数或者其他方式来实现
    
    return result;
}

/**
 * @brief 发送JSON响应
 * @param client_fd 客户端套接字
 * @param result 结果JSON对象，可以为NULL
 * @param error 错误JSON对象，可以为NULL
 * @param id 请求ID
 */
static void send_json_response(int client_fd, cJSON *result, cJSON *error, int id)
{
    cJSON *response = cJSON_CreateObject();
    char *response_str = NULL;
    
    /* 添加result字段 */
    if (result) {
        cJSON_AddItemToObject(response, "result", cJSON_Duplicate(result, 1));
    } else {
        cJSON_AddNullToObject(response, "result");
    }
    
    /* 添加error字段 */
    if (error) {
        cJSON_AddItemToObject(response, "error", cJSON_Duplicate(error, 1));
    } else {
        cJSON_AddNullToObject(response, "error");
    }
    
    /* 添加id字段 */
    cJSON_AddNumberToObject(response, "id", id);
    
    /* 转换为字符串 */
    response_str = cJSON_PrintUnformatted(response);
    if (response_str) {
        /* 发送响应 */
        write(client_fd, response_str, strlen(response_str));
        free(response_str);
    }
    
    /* 清理 */
    cJSON_Delete(response);
}

/**
 * @brief 向监听特定通道的客户端发送数据
 */
int hi_link_ipc_send_to_channel_listeners(const char *channel_name, const char *message)
{
    if (!channel_name || !message) {
        return -1;
    }
    
    int sent_count = 0;
    
    pthread_mutex_lock(&g_clients_mutex);
    
    for (int i = 0; i < g_client_count; i++) {
        if (!g_clients[i].active || g_clients[i].fd <= 0) {
            continue;
        }
        
        // 检查客户端是否监听该通道
        bool is_listening = false;
        for (int j = 0; j < g_clients[i].channel_count; j++) {
            if (strcmp(g_clients[i].listening_channels[j], channel_name) == 0) {
                is_listening = true;
                break;
            }
        }
        
        if (is_listening) {
            ssize_t written = write(g_clients[i].fd, message, strlen(message));
            if (written < 0) {
                IPC_LOG("Failed to send message to client %s (fd:%d): %s\n", 
                       g_clients[i].client_id, g_clients[i].fd, strerror(errno));
                // 标记客户端为非活跃状态
                g_clients[i].active = false;
            } else {
                IPC_LOG("Sent %s data to client %s (fd:%d): %zd bytes\n", 
                       channel_name, g_clients[i].client_id, g_clients[i].fd, written);
                sent_count++;
            }
        }
    }
    
    pthread_mutex_unlock(&g_clients_mutex);
    
    IPC_LOG("Sent %s data to %d listening clients\n", channel_name, sent_count);
    return sent_count > 0 ? 0 : -1;
}

