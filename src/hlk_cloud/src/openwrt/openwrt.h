#ifndef _H_HLK_SUPPORT_OPENWRT_H_
#define _H_HLK_SUPPORT_OPENWRT_H_ 

#include "hi_link.h"

//openwrt平台可以提供的接口

/**
 * @brief OpenWrt平台设置通道值
 * @param channel 通道名称
 * @param value 通道值结构体指针
 * @return 0成功，非0失败
 */
int hi_link_set_channel_value_openwrt(const char *channel, const hi_link_value_t *value);

/**
 * @brief OpenWrt平台获取通道值
 * @param channel 通道名称
 * @param value 通道值结构体指针（输出参数）
 * @return 0成功，非0失败
 */
int hi_link_get_channel_value_openwrt(const char *channel, hi_link_value_t *value);


void openwrt_upgrade_firmware(void);

#endif