/******************************************************************************
                        Copyright (C),  
 ******************************************************************************
  Filename   : hi_link_ipc.h
  Version    : v1.0
  Description: hi_link IPC服务头文件，提供进程间通信接口
 *****************************************************************************/
#ifndef __HI_LINK_IPC_H__
#define __HI_LINK_IPC_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

/*****************************************************************************
 *                                DEFINE                                      *
 *****************************************************************************/
#define HI_LINK_SOCKET_PATH    "/var/run/hi_link.sock"
#define MAX_CLIENTS            1
#define MAX_BUF_SIZE           4096
#define MAX_DEVICE_ID_LEN      32
#define MAX_PRODUCT_ID_LEN     32
#define MAX_CHANNEL_NAME_LEN   64
#define MAX_CHANNELS           2

/* IPC命令类型 */
typedef enum {
    CMD_GET_CONNECTION_STATUS = 0,
    CMD_GET_CHANNEL_DATA,
    CMD_SEND_CHANNEL_DATA,
    CMD_GET_OTA_STATUS,
    CMD_SET_DEVICE_CONFIG,      /* 新增：设置设备配置 */
    CMD_GET_CHANNEL_LIST,       /* 新增：获取通道列表 */
    CMD_UNKNOWN
} hi_link_ipc_cmd_e;

/* IPC服务状态 */
typedef enum {
    IPC_SERVICE_STOPPED = 0,
    IPC_SERVICE_RUNNING,
    IPC_SERVICE_ERROR
} hi_link_ipc_status_e;

/* 通道类型 */
typedef enum {
    CHANNEL_TYPE_STRING = 0,
    CHANNEL_TYPE_NUMBER,
    CHANNEL_TYPE_BOOLEAN
} channel_type_e;

/* 通道配置结构 */
typedef struct {
    char name[MAX_CHANNEL_NAME_LEN];        /* 通道名称 */
    channel_type_e type;                    /* 通道数据类型 */
    bool enabled;                           /* 是否启用 */
    char description[128];                  /* 通道描述 */
    void (*handler)(const char* channel, const cJSON* value); /* 处理函数 */
} channel_config_t;

/* 设备配置结构 */
typedef struct {
    char device_id[MAX_DEVICE_ID_LEN];      /* 设备ID */
    char product_id[MAX_PRODUCT_ID_LEN];    /* 产品ID */
    char device_secret[64];                 /* 设备密钥 */
    channel_config_t channels[MAX_CHANNELS]; /* 通道配置 */
    int channel_count;                      /* 通道数量 */
} device_config_t;

/* 客户端信息结构 */
typedef struct {
    int fd;                                    /* 客户端套接字 */
    char client_id[32];                        /* 客户端标识（如ser2net_1, ser2net_2） */
    char listening_channels[MAX_CHANNELS][MAX_CHANNEL_NAME_LEN]; /* 监听的通道列表 */
    int channel_count;                         /* 监听的通道数量 */
    time_t connect_time;                       /* 连接时间 */
    bool active;                               /* 是否活跃 */
} ipc_client_info_t;


/*****************************************************************************
 *                                FUNCTION                                    *
 *****************************************************************************/
/**
 * @brief 初始化hi_link IPC服务
 * @return 0成功，非0失败
 */
int hi_link_ipc_init(void);

/**
 * @brief 清理hi_link IPC服务
 */
void hi_link_ipc_cleanup(void);

/**
 * @brief 获取IPC服务状态
 * @return IPC服务状态枚举值
 */
hi_link_ipc_status_e hi_link_ipc_get_status(void);

/**
 * @brief 设置设备配置
 * @param config 设备配置
 * @return 0成功，非0失败
 */
int hi_link_ipc_set_device_config(const device_config_t *config);

/**
 * @brief 注册通道处理函数
 * @param channel_name 通道名称
 * @param handler 处理函数
 * @return 0成功，非0失败
 */
int hi_link_ipc_register_channel_handler(const char *channel_name,
                                         void (*handler)(const char*, const cJSON*));

/**
 * @brief 发送消息到通道监听器
 * @param channel_name 通道名称
 * @param message 消息内容
 * @return 0成功，非0失败
 */
int hi_link_ipc_send_to_channel_listeners(const char *channel_name, const char *message);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __HI_LINK_IPC_H__ */