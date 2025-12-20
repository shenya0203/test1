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
