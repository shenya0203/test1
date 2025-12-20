/******************************************************************************

                        Copyright (C),

 ******************************************************************************
  Filename   : hi_link.h
  Version    : v1.0
  Author     :
  Creation   :
  Description:
 *****************************************************************************/
#ifndef __HI_LINK_H__
#define __HI_LINK_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HLK_PRODUCT_WR10
#include "hi_typedef.h"  //WR10的头文件
#include "hi_util_log.h" //WR10的头文件
#endif

#ifdef __cplusplus
#if __cplusplus
extern "C"
{
#endif
#endif /* __cplusplus */

    /*****************************************************************************
     *                                DEFINE                                      *
     *****************************************************************************/

#define DEBUG 1
#if DEBUG
#define PRF(fmt, args...)                                    \
    do                                                       \
    {                                                        \
        printf("< Line:%d  :%s>> ", __LINE__, __FUNCTION__); \
        printf(fmt, ##args);                                 \
    } while (0)
#else
#define DBG_PRINTF(fmt, args...)
#endif

#define HI_U2C_NOTIFIER "u2c_notifier" /**< 用户消息通知 */

#define U2C_INTERVAL 5000 /*5000ms*/

#ifdef HLK_PRODUCT_WR10
#define U2C_ERR(fmt, arg...) hi_error(HI_SUBMODULE_DMS_KEY, "[KEY]" fmt, ##arg)
#else

#define U2C_ERR(fmt, arg...) syslog(LOG_DEBUG, "[KEY]" fmt, ##arg)
#endif

#define HI_U2C_CHECK(_ret, format, arg...)                              \
    do                                                                  \
    {                                                                   \
        hi_uint32 ui_ret = _ret;                                        \
        if (ui_ret)                                                     \
        {                                                               \
            U2C_ERR("Error(0x%x)!!! " #_ret " " format, ui_ret, ##arg); \
        }                                                               \
    } while (0)

    /*****************************************************************************
     *                                TYPEDEF                                    *
     *****************************************************************************/
    typedef enum
    {
        HI_U2C_MSG_WLAN_E = 0,
        HI_U2C_MSG_WPS_E,
        HI_U2C_MSG_REBOOT_E,
        HI_U2C_MSG_SHORT_RESTORE_E,
        HI_U2C_MSG_LONG_RESTORE_E,
        HI_U2C_MSG_LED_PWR_E,
        /* ****** easymesh reference begin ******* */
        HI_U2C_MSG_2G_STA_WPS_E,
        HI_U2C_MSG_5G_STA_WPS_E,
        HI_U2C_MSG_STA_WPS_E,
        /* ****** easymesh reference end ******* */
        HI_U2C_MSG_MAX_E
    } hi_u2c_msg_e;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/*****************************************************************************
 *                            FUNCTION DECLARATIONS                           *
 *****************************************************************************/

/* 通道数据类型枚举 */
typedef enum {
    HI_LINK_TYPE_STRING = 0,
    HI_LINK_TYPE_INT,
    HI_LINK_TYPE_FLOAT,
    HI_LINK_TYPE_BOOL
} hi_link_value_type_e;

#ifndef bool    
#define bool int
#endif

/* 通道值结构体 */
typedef struct {
    hi_link_value_type_e type;
    union {
        char *str_val;      /* 字符串值 */
        int int_val;        /* 整数值 */
        float float_val;    /* 浮点数值 */
        bool bool_val;      /* 布尔值 */
    } data;
} hi_link_value_t;

/**
 * @brief 设置通道值（支持多种数据类型）
 * @param channel 通道名称
 * @param value 通道值结构体指针
 * @return 0成功，非0失败
 */
int hi_link_set_channel_value(const char *channel, const hi_link_value_t *value);

/**
 * @brief 获取通道值（支持多种数据类型）
 * @param channel 通道名称
 * @param value 通道值结构体指针（输出参数）
 * @return 0成功，非0失败
 */
int hi_link_get_channel_value(const char *channel, hi_link_value_t *value);

/**
 * @brief 设置字符串类型通道值（便捷接口）
 * @param channel 通道名称
 * @param str_value 字符串值
 * @return 0成功，非0失败
 */
int hi_link_set_channel_string(const char *channel, const char *str_value);

/**
 * @brief 设置整数类型通道值（便捷接口）
 * @param channel 通道名称
 * @param int_value 整数值
 * @return 0成功，非0失败
 */
int hi_link_set_channel_int(const char *channel, int int_value);

/**
 * @brief 设置浮点数类型通道值（便捷接口）
 * @param channel 通道名称
 * @param float_value 浮点数值
 * @return 0成功，非0失败
 */
int hi_link_set_channel_float(const char *channel, float float_value);

/**
 * @brief 设置布尔类型通道值（便捷接口）
 * @param channel 通道名称
 * @param bool_value 布尔值
 * @return 0成功，非0失败
 */
int hi_link_set_channel_bool(const char *channel, bool bool_value);

/**
 * @brief 获取字符串类型通道值（便捷接口）
 * @param channel 通道名称
 * @param str_value 字符串值缓冲区
 * @param buf_size 缓冲区大小
 * @return 0成功，非0失败
 */
int hi_link_get_channel_string(const char *channel, char *str_value, size_t buf_size);

/**
 * @brief 获取整数类型通道值（便捷接口）
 * @param channel 通道名称
 * @param int_value 整数值指针
 * @return 0成功，非0失败
 */
int hi_link_get_channel_int(const char *channel, int *int_value);

/**
 * @brief 获取浮点数类型通道值（便捷接口）
 * @param channel 通道名称
 * @param float_value 浮点数值指针
 * @return 0成功，非0失败
 */
int hi_link_get_channel_float(const char *channel, float *float_value);

/**
 * @brief 获取布尔类型通道值（便捷接口）
 * @param channel 通道名称
 * @param bool_value 布尔值指针
 * @return 0成功，非0失败
 */
int hi_link_get_channel_bool(const char *channel, bool *bool_value);

/**
 * @brief 释放通道值结构体中的内存
 * @param value 通道值结构体指针
 */
void hi_link_free_channel_value(hi_link_value_t *value);


    /*****************************************************************************
     *                                FUNCTION                                    *
     *****************************************************************************/

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __HI_LINK_H__*/
