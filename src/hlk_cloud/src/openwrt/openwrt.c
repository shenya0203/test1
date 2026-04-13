#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "hi_link.h"
#include "hi_link_ipc.h"
#include "hi_mqtt.h"
#include "hlk_log.h"


//openwrt系统相关接口

int openwrt_ite_test(void)
{
    return 0;
}

int hi_link_set_channel_value_openwrt(const char *channel, const hi_link_value_t *value)
{
    if (!channel || !value) {
        return -1;
    }

    /* 根据通道名称执行不同的设置操作 */
    if (strcmp(channel, "SSID") == 0) {
        if (value->type == HI_LINK_TYPE_STRING && value->data.str_val) {
            HLK_LOG_INFO("Setting WiFi SSID: %s\r\n", value->data.str_val);
            // 调用系统命令设置SSID
            // uci set wireless.@wifi-iface[0].ssid=value->data.str_val
            // uci commit wireless
            // wifi reload
        }
    } else if (strcmp(channel, "WiFiPassword") == 0) {
        if (value->type == HI_LINK_TYPE_STRING && value->data.str_val) {
            HLK_LOG_INFO("Setting WiFi Password\r\n");
            // 调用系统命令设置密码
        }
    } else if (strcmp(channel, "Reboot") == 0) {
        if (value->type == HI_LINK_TYPE_BOOL && value->data.bool_val) {
            HLK_LOG_INFO("Rebooting system...\r\n");
            system("reboot");
        }
    } else if (strcmp(channel, "LEDBrightness") == 0) {
        if (value->type == HI_LINK_TYPE_INT) {
            HLK_LOG_INFO("Setting LED brightness: %d\r\n", value->data.int_val);
            // 设置LED亮度
        }
    }
    
    return 0;
}

int hi_link_get_channel_value_openwrt(const char *channel, hi_link_value_t *value)
{
    if (!channel || !value) {
        return -1;
    }

    /* 根据通道名称获取不同的值 */
    if (strcmp(channel, "SSID") == 0) {
        value->type = HI_LINK_TYPE_STRING;
        // 从系统获取当前SSID
        value->data.str_val = strdup("CurrentSSID"); // 实际应该从uci获取
    } else if (strcmp(channel, "DeviceTemperature") == 0) {
        value->type = HI_LINK_TYPE_FLOAT;
        // 获取设备温度
        value->data.float_val = 45.5; // 示例值
    } else if (strcmp(channel, "OnlineStatus") == 0) {
        value->type = HI_LINK_TYPE_BOOL;
        // 获取在线状态
        value->data.bool_val = true;
    } else {
        return -1; // 未知通道
    }
    
    return 0;
}

void openwrt_upgrade_firmware(void)
{
    HLK_LOG_INFO("openwrt_upgrade_firmware\r\n");
    
    // 方案1：双重fork + execl（最可靠）
    pid_t pid1 = fork();
    if (pid1 == 0) {
        // 第一个子进程
        if (setsid() == -1) {
            HLK_LOG_ERR("setsid failed\r\n");
            exit(1);
        }
        
        pid_t pid2 = fork();
        if (pid2 == 0) {
            // 孙进程 - 完全脱离父进程
            
            // 关闭所有文件描述符
            int max_fd = getdtablesize();
            for (int i = 3; i < max_fd; i++) {
                close(i);
            }
            
            // 重定向标准流到 /dev/null
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, STDIN_FILENO);
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                if (null_fd > 2) close(null_fd);
            }
            
            // 等待父进程完成清理
            sleep(5);
            HLK_LOG_INFO("start upgrade.\n");
            
            // 直接执行 sysupgrade，避免 shell
            execl("/sbin/sysupgrade", "sysupgrade", 
                  "-n",  // 不保存配置
                  SYSUPGRADE_BIN_PATH_TMP, 
                  (char *)NULL);
            
            // 如果 execl 失败，记录日志并退出
            HLK_LOG_ERR("execl sysupgrade failed\r\n");
            exit(1);
            
        } else if (pid2 > 0) {
            // 第一个子进程立即退出，让孙进程被 init 接管
            exit(0);
        } else {
            HLK_LOG_ERR("second fork failed\r\n");
            exit(1);
        }
    } else if (pid1 > 0) {
        // 父进程等待第一个子进程退出，避免僵尸进程
        int status;
        waitpid(pid1, &status, 0);
        HLK_LOG_INFO("Upgrade daemon started\r\n");
    } else {
        HLK_LOG_ERR("first fork failed\r\n");
    }
}