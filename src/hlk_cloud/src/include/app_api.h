/******************************************************************************

                        Copyright (C), 

 ******************************************************************************
  Filename   : hi_link.h
  Version    : v1.0
  Author     : 
  Creation   : 
  Description: 
 *****************************************************************************/
#ifndef __APP_API_H__
#define __APP_API_H__


#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

#include <stdint.h>

/*****************************************************************************
*                                DEFINE                                      *
*****************************************************************************/
#define DEVICE_REBOOT  "reboot"
/*****************************************************************************
 *                                TYPEDEF                                    *
 *****************************************************************************/


/*****************************************************************************
*                                FUNCTION                                    *
*****************************************************************************/
extern int message_print(char* fmt, ...);
#define MESSAGE_PRINT(fmt, args...) message_print(fmt,##args) /*lint -e1776*/

extern int conver_hex2str(char *in, int in_len, char *out, int out_len);
extern int conver_str2hex(char *in, int in_len, char *out, int out_len);

extern void app_msleep(unsigned int msec);

// Zig实现的毫秒休眠函数，避免C ABI兼容性问题
extern void zig_msleep(unsigned int msec);

// Zig实现的时间戳获取函数，避免C ABI兼容性问题
extern long long zig_get_timestamp(void);

// Zig实现的时区设置函数，避免C ABI兼容性问题
extern int zig_set_timezone(const char *tz);

// Zig实现的system()时区设置函数，直接使用system调用
extern int zig_set_timezone_system(const char *tz);

// Zig实现的绝对时间差值计算函数，避免fabs阻塞
extern long long zig_time_diff_abs(long long time1, long long time2);

// Zig实现的时间同步设置函数，避免C ABI兼容性问题
extern int zig_set_timesync(long long timestamp);

// Zig实现的gettimeofday函数，避免C ABI兼容性问题
typedef struct {
    long tv_sec;
    long tv_usec;
} zig_timeval;

extern int zig_gettimeofday(zig_timeval *tv, void *tz);

// Zig实现的select函数，避免C ABI兼容性问题
typedef struct {
    unsigned int fds_bits[16];
} zig_fd_set;

extern int zig_select(int nfds, zig_fd_set *readfds, zig_fd_set *writefds, zig_fd_set *exceptfds, zig_timeval *timeout);

// fd_set操作函数
extern void zig_FD_ZERO(zig_fd_set *set);
extern void zig_FD_SET(int fd, zig_fd_set *set);
extern int zig_FD_ISSET(int fd, zig_fd_set *set);
extern uint32_t hlk_sntp_time_get(void);

extern void get_mem_info(unsigned long *total_memory,unsigned long *free_memory);
extern void get_disk_info(unsigned long *disk_size,unsigned long *free_disk_size);
extern void get_cpu_info(float *cpu_rate);
extern void get_temp_info(double *temperature); 
extern void get_battery_info(double *battery); 
extern void get_local_ip(char *local_ip); 
extern void get_uptime_info(unsigned int *uptime); 
extern void get_utc_time_info(unsigned long *utc_time); 
extern void get_delay_info(unsigned int *delay); 
extern void get_module_info(char *module); 
extern void get_version_info(char *version); 
extern char *get_imei_info(char *imei_data);
extern char *get_iccid_info(char *iccid_data);
extern char *get_imsi_info(char *imsi_data);

extern int get_interface_mac(char *iface,char *mac);
extern void getWanIpAddress(char* interfaceName, char* ipAddress) ;

extern void app_reboot(void);
extern int SYSTEM(const char *command);
extern int set_timesync(long long timestamp);

extern int mkdirs(const char *dir);
#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __APP_API_H__*/
