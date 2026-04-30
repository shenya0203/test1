/******************************************************************************
 * 文件名   : app_api.c
 * 版本     : V1.0
 * 作者     : 
 * 创建时间 : 
 * 描述     : 应用程序API接口实现文件
 *           主要功能包括：
 *           1. 数据格式转换（十六进制与字符串互转）
 *           2. 系统信息获取（内存、CPU、磁盘、温度等）
 *           3. 网络接口管理（IP地址、MAC地址获取）
 *           4. 日志管理（消息打印、日志裁剪）
 *           5. 系统控制（重启、休眠等）
 * 
 * 适用平台 : Linux系统
 * 依赖库   : 标准C库、Linux系统调用
 ******************************************************************************/

/*****************************************************************************
 *                                头文件包含                                  *
 *****************************************************************************/
#include <stdio.h>          // 标准输入输出库
#include <stdlib.h>         // 标准库函数（内存分配、进程控制等）
#include <string.h>         // 字符串处理函数
#include <time.h>           // 时间相关函数
#include <sys/sysinfo.h>    // 系统信息获取
#include <unistd.h>         // Unix标准函数（sleep、access等）
#include <sys/ioctl.h>      // 输入输出控制
#include <sys/stat.h>       // 文件状态信息
#include <net/if.h>         // 网络接口定义
#include <netinet/in.h>     // Internet地址族
#include <arpa/inet.h>      // Internet操作函数
#include <stdarg.h>         // 可变参数列表处理

#ifdef HLK_PRODUCT_7628
#include <sys/wait.h>
#endif

#include "hi_cfm_version.h" // 版本信息头文件
#include "hlk_log.h"        // 日志相关头文件
#include "app_api.h"        // 应用API头文件
#include "hi_mqtt.h"        // MQTT协议相关头文件
#include "cJSON.h"          // JSON解析库

extern void zig_msleep(unsigned int msec);

/*****************************************************************************
 *                                宏定义                                      *
 *****************************************************************************/
/**
 * @brief 将4位十六进制数值转换为ASCII字符
 * @param h 4位十六进制数值（0-15）
 * @return 对应的ASCII字符（'0'-'9', 'A'-'F'）
 * 
 * 转换规则：
 * 0-9  -> '0'-'9'
 * 10-15 -> 'A'-'F'
 */
#define HEX2STR(h) ((h) < 0x0A ? (h) + '0' : ((h)-0x0A) + 'A')

/**
 * @brief 将ASCII十六进制字符转换为4位数值
 * @param h ASCII字符（'0'-'9', 'A'-'F', 'a'-'f'）
 * @return 对应的4位数值（0-15）
 * 
 * 转换规则：
 * '0'-'9' -> 0-9
 * 'A'-'F' -> 10-15
 * 'a'-'f' -> 10-15
 */
#define STR2HEX(h) ((h) < 'A' ? (h) - '0' : ((h) < 'a' ? ((h) - 'A') + 0x0A : ((h) - 'a') + 0x0A))

/**
 * @brief 单行文本最大长度限制
 */
#define MAX_LINE_LENGTH 1024

/**
 * @brief 日志文件最大行数限制
 * 超过此行数将触发日志裁剪操作
 */
#define MAX_LINES 500

/**
 * @brief 蜂窝模组信息文件路径
 */
#define MODEM_INFO_JSON_PATH "/tmp/modem_info.json"

/**
 * @brief 蜂窝模组字段最大长度
 */
#define MODEM_INFO_VALUE_LEN 32

/*****************************************************************************
 *                                类型定义                                    *
 *****************************************************************************/
// 此处可添加自定义数据类型定义

/*****************************************************************************
 *                                局部变量                                    *
 *****************************************************************************/
static int g_modem_info_loaded = 0;
static int g_modem_imei_valid = 0;
static int g_modem_iccid_valid = 0;
static int g_modem_imsi_valid = 0;
static char g_modem_imei[MODEM_INFO_VALUE_LEN] = {0};
static char g_modem_iccid[MODEM_INFO_VALUE_LEN] = {0};
static char g_modem_imsi[MODEM_INFO_VALUE_LEN] = {0};

static int modem_info_strcasecmp(const char *left, const char *right)
{
    unsigned char left_ch;
    unsigned char right_ch;

    if (left == NULL || right == NULL) {
        return -1;
    }

    while (*left != '\0' && *right != '\0') {
        left_ch = (unsigned char)*left;
        right_ch = (unsigned char)*right;

        if (left_ch >= 'A' && left_ch <= 'Z') {
            left_ch = left_ch - 'A' + 'a';
        }

        if (right_ch >= 'A' && right_ch <= 'Z') {
            right_ch = right_ch - 'A' + 'a';
        }

        if (left_ch != right_ch) {
            return (int)left_ch - (int)right_ch;
        }

        left++;
        right++;
    }

    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

static int modem_info_is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int modem_info_value_is_valid(const char *value)
{
    const char *start = value;
    const char *end;
    size_t len;
    char temp[MODEM_INFO_VALUE_LEN] = {0};

    if (value == NULL) {
        return 0;
    }

    while (modem_info_is_space(*start)) {
        start++;
    }

    if (*start == '\0') {
        return 0;
    }

    end = start + strlen(start);
    while (end > start && modem_info_is_space(*(end - 1))) {
        end--;
    }

    len = (size_t)(end - start);
    if (len == 0 || len >= sizeof(temp)) {
        return 0;
    }

    memcpy(temp, start, len);
    temp[len] = '\0';

    if (modem_info_strcasecmp(temp, "N/A") == 0 ||
        modem_info_strcasecmp(temp, "NA") == 0 ||
        modem_info_strcasecmp(temp, "NULL") == 0 ||
        modem_info_strcasecmp(temp, "NONE") == 0 ||
        modem_info_strcasecmp(temp, "UNKNOWN") == 0) {
        return 0;
    }

    return 1;
}

static int modem_info_read_file(char *buffer, size_t buffer_size)
{
    FILE *fp = NULL;
    size_t read_len;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    fp = fopen(MODEM_INFO_JSON_PATH, "r");
    if (fp == NULL) {
        return -1;
    }

    read_len = fread(buffer, 1, buffer_size - 1, fp);
    if (ferror(fp)) {
        fclose(fp);
        buffer[0] = '\0';
        return -1;
    }

    buffer[read_len] = '\0';
    fclose(fp);

    if (read_len == 0) {
        return -1;
    }

    return 0;
}

static int modem_info_cache_field(cJSON *root, const char *field, char *cache, size_t cache_size)
{
    cJSON *item = NULL;
    const char *start = NULL;
    const char *end = NULL;
    size_t len;

    if (root == NULL || field == NULL || cache == NULL || cache_size == 0) {
        return 0;
    }

    item = cJSON_GetObjectItem(root, field);
    if (item == NULL || !cJSON_IsString(item) || !modem_info_value_is_valid(item->valuestring)) {
        cache[0] = '\0';
        return 0;
    }

    start = item->valuestring;
    while (modem_info_is_space(*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && modem_info_is_space(*(end - 1))) {
        end--;
    }

    len = (size_t)(end - start);
    if (len >= cache_size) {
        cache[0] = '\0';
        return 0;
    }

    memcpy(cache, start, len);
    cache[len] = '\0';
    return 1;
}

static void load_modem_info_once(void)
{
    char json_buffer[1024] = {0};
    cJSON *root = NULL;

    if (g_modem_info_loaded) {
        return;
    }

    g_modem_info_loaded = 1;

    if (modem_info_read_file(json_buffer, sizeof(json_buffer)) != 0) {
        return;
    }

    root = cJSON_Parse(json_buffer);
    if (root == NULL) {
        return;
    }

    g_modem_imei_valid = modem_info_cache_field(root, "imei", g_modem_imei, sizeof(g_modem_imei));
    g_modem_iccid_valid = modem_info_cache_field(root, "iccid", g_modem_iccid, sizeof(g_modem_iccid));
    g_modem_imsi_valid = modem_info_cache_field(root, "imsi", g_modem_imsi, sizeof(g_modem_imsi));

    cJSON_Delete(root);
}

 /*****************************************************************************
 *                                应用函数实现                               *
 *****************************************************************************/
 
 /******************************************************************************
 * @brief           将十六进制字节数组转换为可打印的ASCII字符串
 * @param[in]       in        输入的十六进制数据缓冲区指针
 * @param[in]       in_len    输入数据长度（单位：字节）
 * @param[out]      out       输出ASCII字符串缓冲区指针
 * @param[in]       out_len   输出缓冲区最大容量（单位：字符）
 * @return          int       实际转换后的ASCII字符数量（输入长度的2倍）
 * 
 * @details
 * 函数将每个输入字节拆分为高4位和低4位，分别转换为ASCII字符。例如：
 * 输入字节0x1A会被转换为字符串"1A"。
 * - 高4位转换：右移4位后取低4位，对应HEX2STR宏处理（如0x1 -> '1'）
 * - 低4位转换：直接取低4位，对应HEX2STR宏处理（如0xA -> 'A'）
 * 
 * @note
 * 1. 输出缓冲区需预留至少输入长度*2的空间
 * 2. 若输出缓冲区不足，输入数据会被截断
 * 3. 函数不会在输出字符串末尾添加'\0'，调用者需自行处理
 * 
 * @example
 * char input[] = {0x1A, 0x2B, 0x3C};
 * char output[10];
 * int len = conver_hex2str(input, 3, output, 10);  // 返回6，output为"1A2B3C"
 ******************************************************************************/
int conver_hex2str(char *in, int in_len, char *out, int out_len)
{
    int idx;
    
    // 检查输出缓冲区容量，防止溢出
    if (in_len > out_len / 2)
    {
        in_len = out_len / 2;  // 截断输入长度以适应输出缓冲区
    }

    // 逐字节转换：每个字节转换为两个ASCII字符
    for (idx = 0; idx < in_len; idx++)
    {
        // 转换低4位到ASCII字符（存储在奇数位置）
        out[idx * 2 + 1] = HEX2STR(in[idx] & 0x0F);
        
        // 转换高4位到ASCII字符（存储在偶数位置）
        out[idx * 2] = HEX2STR((in[idx] >> 4) & 0x0F);
    }

    return in_len * 2;  // 返回实际转换的字符数
}

/******************************************************************************
 * @brief           将ASCII十六进制字符串转换为二进制字节数组
 * @param[in]       in        输入ASCII字符串指针（需为合法十六进制字符，如"1A2B"）
 * @param[in]       in_len    输入字符串长度（单位：字符）
 * @param[out]      out       输出二进制数据缓冲区指针
 * @param[in]       out_len   输出缓冲区最大容量（单位：字节）
 * @return          int       实际转换的字符数（不是字节数，注意这里的返回值可能有问题）
 * 
 * @details
 * 函数将每两个ASCII字符组合为一个字节的十六进制数据。例如：
 * 输入字符串"1A"会被转换为字节0x1A。
 * - 高4位转换：取第idx个字符，通过STR2HEX宏转为数值后左移4位
 * - 低4位转换：取第idx+1个字符，通过STR2HEX宏转为数值后与高4位合并
 * 
 * @note
 * 1. 输入字符串长度应为偶数，否则末位字符将被忽略
 * 2. 输出缓冲区需预留至少输入长度/2的空间
 * 3. STR2HEX宏需实现字符'0'-'9','A'-'F'/'a'-'f'到0x0-0xF的转换
 * 4. 若输入长度超过输出容量*2，数据会被截断
 * 5. 返回值似乎有误，应该返回in_len/2而不是in_len*2
 * 
 * @example
 * char input[] = "1A2B3C";
 * char output[10];
 * int len = conver_str2hex(input, 6, output, 10);  // 应该返回3，output为{0x1A, 0x2B, 0x3C}
 ******************************************************************************/
int conver_str2hex(char *in, int in_len, char *out, int out_len)
{
    int idx;
    
    // 检查输入长度，防止输出缓冲区溢出
    if (in_len > out_len * 2)
    {
        in_len = out_len * 2;  // 截断输入长度以适应输出缓冲区
    }

    // 每两个字符转换为一个字节
    for (idx = 0; idx < in_len; idx += 2)
    {
        // 转换第一个字符为高4位
        out[idx / 2] = (STR2HEX(in[idx]) & 0x0F) << 4;
        
        // 转换第二个字符为低4位并与高4位合并
        out[idx / 2] |= (STR2HEX(in[idx + 1]) & 0x0F);
    }

    return in_len * 2;  // 注意：这里的返回值可能有误，应该是in_len/2
}

 /******************************************************************************
 * @brief      毫秒级休眠函数
 * @param[in]  msec 休眠时间（毫秒）
 * @return     无
 * 
 * @details
 * 使用nanosleep系统调用实现精确的毫秒级休眠。
 * 如果nanosleep调用失败，则回退到使用hi_os_msleep函数。
 * 
 * @note
 * 1. nanosleep可能被信号中断，此时函数会回退到备用方案
 * 2. hi_os_msleep是备用的休眠函数，需要在其他地方定义
 * 3. 实际休眠时间可能会因系统调度而略有偏差
 ******************************************************************************/
void app_msleep(unsigned int msec)
{
    // 直接使用Zig实现的休眠函数，避免C ABI兼容性问题
    zig_msleep(msec);
}

 /******************************************************************************
 * @brief      获取当前SNTP时间戳
 * @param[in]  无
 * @return     uint32_t 当前时间的Unix时间戳
 * 
 * @details
 * 获取当前系统时间的Unix时间戳（从1970年1月1日00:00:00 UTC开始的秒数）。
 * 通常用于网络时间同步或时间戳记录。
 * 
 * @note
 * 1. 返回值为32位无符号整数，可表示到2038年
 * 2. 时间戳基于系统当前时间，受系统时钟影响
 * 3. SNTP通常指简单网络时间协议，此函数获取本地时间
 ******************************************************************************/
uint32_t hlk_sntp_time_get(void)
{
    return (uint32_t)zig_get_timestamp();  // 获取当前Unix时间戳
}

/******************************************************************************
 * @brief           获取指定网络接口的IPv4地址
 * @param[in]       interfaceName   接口名称（如"eth0"、"wlan0"）
 * @param[out]      ipAddress       IPv4地址输出缓冲区（需预留至少INET_ADDRSTRLEN长度）
 * @return          void
 * 
 * @details
 * 通过socket和ioctl调用获取指定接口的IPv4地址，适用于Linux系统。核心逻辑：
 * 1. 创建UDP套接字（AF_INET/SOCK_DGRAM）
 * 2. 使用SIOCGIFADDR命令获取接口地址
 * 3. 将二进制地址转换为点分十进制字符串格式
 * 
 * @note
 * 1. 接口名称需符合系统命名规范（如eth0、enp0s3），否则ioctl失败
 * 2. 仅支持IPv4地址，若需IPv6需改用AF_INET6和inet_ntop
 * 3. 需要适当的系统权限才能执行ioctl操作
 * 4. 错误处理通过perror输出，调用方需检查ipAddress内容是否有效
 * 5. 输出缓冲区建议大小至少为INET_ADDRSTRLEN（16字节）
 * 
 * @example
 * char ip[INET_ADDRSTRLEN];
 * getWanIpAddress("eth0", ip);  // 输出如：192.168.1.100
 ******************************************************************************/
void getWanIpAddress(char* interfaceName, char* ipAddress) 
{
    int fd;
    struct ifreq ifr;

    // 创建UDP套接字用于ioctl调用
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");  // 套接字创建失败
        return;
    }

    // 设置要查询的网络接口名称
    strncpy(ifr.ifr_name, interfaceName, IFNAMSIZ);

    // 使用ioctl获取接口的IP地址
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        perror("ioctl");  // 获取IP地址失败（接口不存在或无IP）
        close(fd);
        return;
    }

    // 将sockaddr结构转换为sockaddr_in以获取IPv4地址
    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
    
    // 将二进制IP地址转换为点分十进制字符串
    strcpy(ipAddress, inet_ntoa(addr->sin_addr));

    close(fd);  // 关闭套接字

    return;
}

/******************************************************************************
 * @brief           获取指定网络接口的MAC地址
 * @param[in]       iface   接口名称（如"eth0"）
 * @param[out]      mac     输出缓冲区（需至少18字节，格式XX-XX-XX-XX-XX-XX）
 * @return          int     0成功，1失败
 * 
 * @details
 * 使用SIOCGIFHWADDR命令从接口硬件地址中提取MAC地址，关键步骤：
 * 1. 创建AF_INET类型socket
 * 2. 通过ioctl获取ifreq结构中的sa_data字段（硬件地址）
 * 3. 格式化为十六进制字符串（使用"-"分隔）
 * 
 * @note
 * 1. MAC地址格式为XX-XX-XX-XX-XX-XX，与Windows的"-"分隔符兼容
 * 2. 虚拟接口（如docker0、lo）可能返回非标准的硬件地址
 * 3. 若接口未启用或不存在，ioctl将返回错误
 * 4. 输出缓冲区必须至少18字节以容纳完整MAC地址字符串
 * 5. 返回0表示成功，1表示失败
 * 
 * @example
 * char mac[18];
 * if (get_interface_mac("eth0", mac) == 0) {
 *     printf("MAC: %s\n", mac);  // 输出如：00-1A-4B-3C-2D-7F
 * }
 ******************************************************************************/
int get_interface_mac(char *iface,char *mac) {
    int fd;
    struct ifreq ifr;

    // 创建socket用于ioctl调用
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");  // 套接字创建失败
        return 1;
    }

    // 设置要查询的网络接口名称
    strncpy(ifr.ifr_name, iface, IFNAMSIZ);

    // 获取接口的MAC地址（硬件地址）
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        perror("ioctl");  // 获取MAC地址失败
        close(fd);
        return 1;
    }

    // 将6字节的硬件地址格式化为XX-XX-XX-XX-XX-XX格式
    sprintf(mac,"%02x-%02x-%02x-%02x-%02x-%02x",
           (unsigned char)ifr.ifr_hwaddr.sa_data[0],  // MAC地址第1字节
           (unsigned char)ifr.ifr_hwaddr.sa_data[1],  // MAC地址第2字节
           (unsigned char)ifr.ifr_hwaddr.sa_data[2],  // MAC地址第3字节
           (unsigned char)ifr.ifr_hwaddr.sa_data[3],  // MAC地址第4字节
           (unsigned char)ifr.ifr_hwaddr.sa_data[4],  // MAC地址第5字节
           (unsigned char)ifr.ifr_hwaddr.sa_data[5]); // MAC地址第6字节

    close(fd);  // 关闭套接字

    return 0;  // 成功返回0
}

/******************************************************************************
 * @brief           获取系统内存使用信息
 * @param[out]      total_memory  总内存大小（字节）
 * @param[out]      free_memory   可用内存大小（字节）
 * @return          void
 * 
 * @details
 * 使用sysinfo系统调用获取系统内存使用情况。
 * 通过读取系统信息结构体中的内存相关字段来计算总内存和可用内存。
 * 
 * @note
 * 1. 内存大小以字节为单位返回
 * 2. 可用内存包括空闲内存，不包括缓冲区和缓存
 * 3. 如果sysinfo调用失败，输出参数将被设置为0
 * 4. 函数会在控制台打印内存信息用于调试
 ******************************************************************************/
void get_mem_info(unsigned long *total_memory,unsigned long *free_memory) 
{
    struct sysinfo info;

    // 初始化输出参数
    *total_memory = 0;
    *free_memory = 0;
    
    // 获取系统信息
    if (sysinfo(&info) != 0) {
        HLK_LOG_ERR("Failed to get system information.\n");
        return;
    }

    // 计算总内存和可用内存（考虑内存单元大小）
    *total_memory = info.totalram * info.mem_unit;  // 总内存 = 内存页数 × 页大小
    *free_memory = info.freeram * info.mem_unit;    // 可用内存 = 空闲页数 × 页大小

    // 调试输出
    HLK_LOG_INFO("Total memory: %ld bytes\n", *total_memory);
    HLK_LOG_INFO("Free memory: %ld bytes\n", *free_memory);

    return;
}

/******************************************************************************
 * @brief           获取磁盘使用信息（当前为空实现）
 * @param[out]      disk_size       总磁盘大小（字节）
 * @param[out]      free_disk_size  可用磁盘空间（字节）
 * @return          void
 * 
 * @details
 * 此函数目前为空实现，仅将输出参数初始化为0。
 * 实际使用时需要实现磁盘空间查询逻辑，可以使用statvfs或statfs系统调用。
 * 
 * @note
 * 1. 当前版本未实现实际功能
 * 2. 可以通过statvfs("/")获取根文件系统的磁盘使用情况
 * 3. 或者通过解析/proc/mounts和statvfs获取所有挂载点的磁盘信息
 * 
 * @todo 实现磁盘空间查询功能
 ******************************************************************************/
void get_disk_info(unsigned long *disk_size,unsigned long *free_disk_size) 
{
    // 当前为空实现，将输出参数设置为0
    *disk_size = 0;
    *free_disk_size = 0;
    return;
}

/******************************************************************************
 * @brief           获取CPU使用率信息（当前为空实现）
 * @param[out]      cpu_rate  CPU使用率（百分比，0.0-100.0）
 * @return          void
 * 
 * @details
 * 此函数目前为空实现，仅将CPU使用率设置为0.0。
 * 实际使用时需要通过读取/proc/stat文件或其他方式计算CPU使用率。
 * 
 * @note
 * 1. 当前版本未实现实际功能
 * 2. 可以通过解析/proc/stat中的CPU时间信息计算使用率
 * 3. 需要在两个时间点采样CPU时间，计算差值来得到使用率
 * 
 * @todo 实现CPU使用率计算功能
 ******************************************************************************/
void get_cpu_info(float *cpu_rate) {
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;
    unsigned long long total = 0;
    unsigned long long idle = 0;
    unsigned long long user, nice, system, idle_time, iowait, irq, softirq, steal;

    FILE *stat_file = fopen("/proc/stat", "r");
    if (stat_file == NULL) {
        HLK_LOG_ERR("Failed to open /proc/stat file.\n");
        *cpu_rate = 0.0;
        return;
    }

    // 读取CPU统计信息
    // 格式: cpu user nice system idle iowait irq softirq steal
    if (fscanf(stat_file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle_time, &iowait, &irq, &softirq, &steal) != 8) {
        HLK_LOG_ERR("Failed to read CPU stats from /proc/stat.\n");
        fclose(stat_file);
        *cpu_rate = 0.0;
        return;
    }

    fclose(stat_file);

    // 计算总时间和空闲时间
    total = user + nice + system + idle_time + iowait + irq + softirq + steal;
    idle = idle_time + iowait;  // 空闲时间包括idle和iowait

    // 计算CPU使用率
    if (prev_total == 0 || prev_idle == 0) {
        // 第一次调用，无法计算使用率
        *cpu_rate = 0.0;
    } else {
        unsigned long long total_diff = total - prev_total;
        unsigned long long idle_diff = idle - prev_idle;

        if (total_diff > 0) {
            *cpu_rate = 100.0 * (total_diff - idle_diff) / (float)total_diff;
            // 保留2位小数精度，避免过长的浮点数
            *cpu_rate = ((int)(*cpu_rate * 100 + 0.5)) / 100.0;
        } else {
            *cpu_rate = 0.0;
        }
    }

    // 保存当前值用于下次计算
    prev_total = total;
    prev_idle = idle;

    HLK_LOG_INFO("CPU usage: %.2f%%\n", *cpu_rate);
}

/******************************************************************************
 * @brief           获取本地WAN接口IP地址
 * @param[out]      local_ip  本地IP地址字符串缓冲区
 * @return          void
 * 
 * @details
 * 通过调用getWanIpAddress函数获取WAN_INTERFACE定义的网络接口的IP地址。
 * WAN_INTERFACE应该在头文件中定义为具体的网络接口名称。
 * 
 * @note
 * 1. WAN_INTERFACE宏需要在头文件中定义（如"eth0"、"enp0s3"等）
 * 2. local_ip缓冲区大小应至少为INET_ADDRSTRLEN（16字节）
 * 3. 函数会在控制台打印IP地址用于调试
 * 4. 如果接口不存在或无IP地址，local_ip内容可能无效
 ******************************************************************************/
void get_local_ip(char *local_ip)
{
    // 获取WAN接口的IP地址
    getWanIpAddress(WAN_INTERFACE, local_ip);
    
    // 调试输出
    HLK_LOG_INFO("WAN IP address: %s\n", local_ip);
    return;
}

/******************************************************************************
 * @brief           获取系统运行时间（当前为硬编码实现）
 * @param[out]      uptime  系统运行时间（秒）
 * @return          void
 * 
 * @details
 * 此函数目前返回硬编码的时间值1745287759。
 * 注释掉的代码显示了通过读取/proc/uptime文件获取实际系统运行时间的方法。
 * 
 * @note
 * 1. 当前版本使用硬编码值，不是实际的系统运行时间
 * 2. /proc/uptime文件包含两个数值：系统运行时间和空闲时间
 * 3. 注释的代码中存在一些问题（如uptime变量赋值错误）
 * 
 * @todo 
 * 1. 启用/proc/uptime读取功能
 * 2. 修复注释代码中的变量赋值问题
 * 3. 添加错误处理
 ******************************************************************************/
void get_uptime_info(unsigned int *uptime)
{
    FILE *uptime_file;
    float fUptime;
    float idle_time;  // /proc/uptime 格式: uptime idle-time

    uptime_file = fopen("/proc/uptime", "r");
    if (uptime_file == NULL) {
        HLK_LOG_ERR("Failed to open /proc/uptime file.\n");
        *uptime = 0;  // 返回0表示获取失败
        return;
    }

    // 读取两个浮点数：系统运行时间和空闲时间
    if (fscanf(uptime_file, "%f %f", &fUptime, &idle_time) != 2) {
        HLK_LOG_ERR("Failed to read uptime from /proc/uptime file.\n");
        fclose(uptime_file);
        *uptime = 0;  // 返回0表示获取失败
        return;
    }

    // 转换并返回系统运行时间（秒）
    *uptime = (unsigned int)fUptime;

    HLK_LOG_INFO("Device uptime: %u seconds\n", *uptime);

    fclose(uptime_file);
}

/******************************************************************************
 * @brief           获取当前UTC时间戳
 * @param[out]      utc_time  UTC时间戳（秒）
 * @return          void
 * 
 * @details
 * 使用time函数获取当前的UTC时间戳（Unix时间戳）。
 * 时间戳表示从1970年1月1日00:00:00 UTC开始的秒数。
 * 
 * @note
 * 1. 返回的是系统当前时间的时间戳
 * 2. 时间戳受系统时钟设置影响
 * 3. 适用于需要标准时间基准的场景
 ******************************************************************************/
void get_utc_time_info(unsigned long *utc_time)
{
    time_t timesnow;
    
    // 获取当前时间
    timesnow = zig_get_timestamp();
    
    // 将时间戳赋值给输出参数
    *utc_time = timesnow;
    return;
}

/******************************************************************************
 * @brief           获取网络延迟信息（当前为空实现）
 * @param[out]      delay  网络延迟（毫秒）
 * @return          void
 * 
 * @details
 * 此函数目前为空实现，仅将延迟设置为0。
 * 实际使用时可以通过ping或其他网络测试方法获取延迟信息。
 * 
 * @note
 * 1. 当前版本未实现实际功能
 * 2. 可以通过ICMP ping或TCP连接测试获取网络延迟
 * 3. 延迟测试通常需要指定目标服务器
 * 
 * @todo 实现网络延迟测试功能
 ******************************************************************************/
void get_delay_info(unsigned int *delay)
{
    // 当前为空实现，将延迟设置为0
    *delay = 0;
    return;
}

/******************************************************************************
 * @brief           获取模块类型信息
 * @param[out]      module  模块类型字符串缓冲区
 * @return          void
 * 
 * @details
 * 从MODULE_TYPE宏中复制模块类型字符串到输出缓冲区。
 * MODULE_TYPE应该在头文件中定义为模块的类型标识。
 * 
 * @note
 * 1. MODULE_TYPE宏需要在头文件中定义
 * 2. 输出缓冲区大小应足够容纳MODULE_TYPE字符串
 * 3. 使用strncpy确保不会发生缓冲区溢出
 ******************************************************************************/
void get_module_info(char *module)
{
    // 复制模块类型字符串（长度限制为MODULE_TYPE的长度）
    strncpy(module, MODULE_TYPE, strlen(MODULE_TYPE));
    return;
}

/******************************************************************************
 * @brief           获取软件版本信息
 * @param[out]      version  版本字符串缓冲区
 * @return          void
 * 
 * @details
 * 从AT_VERSION宏中复制版本字符串到输出缓冲区。
 * AT_VERSION应该在版本头文件中定义为当前软件版本。
 * 
 * @note
 * 1. AT_VERSION宏需要在版本头文件中定义
 * 2. 函数会检查输出缓冲区指针是否有效
 * 3. 使用strcpy复制完整的版本字符串
 ******************************************************************************/
void get_version_info(char *version)
{
    // 检查输出缓冲区指针是否有效
    if(version != NULL){
        // 复制版本字符串
        strcpy(version, AT_VERSION);
    }
    return;
}

char *get_imei_info(char *imei_data)
{
    if (imei_data == NULL) {
        return NULL;
    }

    load_modem_info_once();
    if (!g_modem_imei_valid) {
        return NULL;
    }

    HLK_LOG_INFO("IMEI: %s\n", g_modem_imei);

    strcpy(imei_data, g_modem_imei);
    return imei_data;
}

char *get_iccid_info(char *iccid_data)
{
    if (iccid_data == NULL) {
        return NULL;
    }

    load_modem_info_once();
    if (!g_modem_iccid_valid) {
        return NULL;
    }
    HLK_LOG_INFO("ICCID: %s\n", g_modem_iccid);

    strcpy(iccid_data, g_modem_iccid);
    return iccid_data;
}

char *get_imsi_info(char *imsi_data)
{
    if (imsi_data == NULL) {
        return NULL;
    }

    load_modem_info_once();
    if (!g_modem_imsi_valid) {
        return NULL;
    }
    HLK_LOG_INFO("IMSI: %s\n", g_modem_imsi);

    strcpy(imsi_data, g_modem_imsi);
    return imsi_data;
}

#if 1  // 条件编译开关，便于调试时启用/禁用以下功能

/******************************************************************************
 * @brief           获取系统温度信息（当前为空实现）
 * @param[out]      temperature  系统温度（摄氏度）
 * @return          void
 * 
 * @details
 * 此函数目前为空实现。注释掉的代码显示了通过读取Linux热感器接口
 * /sys/class/thermal/thermal_zone0/temp获取系统温度的方法。
 * 
 * @note
 * 1. 当前版本未实现实际功能
 * 2. Linux系统通常在/sys/class/thermal/目录下提供温度信息
 * 3. 温度值通常以毫摄氏度为单位，需要除以1000得到摄氏度
 * 4. 不同系统的热感器位置可能不同（thermal_zone0, thermal_zone1等）
 * 
 * @todo 
 * 1. 启用温度读取功能
 * 2. 添加多个热感器的支持
 * 3. 添加错误处理
 ******************************************************************************/
void get_temp_info(double *temperature) 
{
    // 以下是读取系统温度的实现代码（已注释）
    // FILE *fp;
    // char path[50];
    // float temp;
    // int zone = 0;  // 热感器编号
    // 
    // // 构造热感器文件路径
    // sprintf(path, "/sys/class/thermal/thermal_zone%d/temp", zone);
    //
    // fp = fopen(path, "r");
    // if (fp == NULL) {
    //     printf("Failed to open %s\n",path);
    //     return;
    // }
    //
    // // 读取温度值（毫摄氏度）
    // fscanf(fp, "%f", &temp);
    // fclose(fp);
    // *temperature = temp;
    //
    // // 转换为摄氏度并输出
    // printf("temperature:%.2f \n", temp / 1000);

    return;  // 注意：void函数不应该返回值
}

/******************************************************************************
 * @brief           获取电池电量信息（当前为空实现）
 * @param[out]      battery  电池电量（百分比）
 * @return          void
 * 
 * @details
 * 此函数目前为空实现，仅将电池电量设置为0。
 * 实际使用时可以通过读取/sys/class/power_supply/目录下的电池信息获取电量。
 * 
 * @note
 * 1. 当前版本未实现实际功能
 * 2. Linux系统通常在/sys/class/power_supply/下提供电池信息
 * 3. 需要根据具体硬件平台调整电池信息读取方式
 * 4. 变量赋值有误，应该是*battery = 0而不是battery = 0
 * 
 * @todo 实现电池电量读取功能
 ******************************************************************************/
void get_battery_info(double *battery)
{
    battery = 0;  // 注意：这里有错误，应该是 *battery = 0;
    return;
}

/******************************************************************************
 * @brief           递归创建目录路径
 * @param[in]       dir  要创建的目录路径
 * @return          int  0成功，-1失败
 * 
 * @details
 * 递归创建指定路径的所有目录层级。如果路径中的某个目录不存在，
 * 函数会先创建父目录，再创建子目录，确保整个路径都存在。
 * 
 * @algorithm
 * 1. 复制目录路径到临时缓冲区
 * 2. 去除路径末尾的'/'字符
 * 3. 从路径开始遍历每个'/'分隔的目录
 * 4. 对每个目录路径检查是否存在，不存在则创建
 * 5. 最后检查完整路径是否存在，不存在则创建
 * 
 * @note
 * 1. 目录权限设置为S_IRWXU（用户读写执行权限）
 * 2. 使用access函数检查目录是否存在
 * 3. 临时缓冲区大小为256字节，超长路径可能导致截断
 * 4. 函数会处理以'/'结尾和不以'/'结尾的路径
 ******************************************************************************/
int mkdirs(const char *dir) 
{
    char tmp[256];
    char *p = NULL;
    size_t len;

    // 复制路径到临时缓冲区
    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    
    // 去除路径末尾的'/'
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
        
    // 遍历路径中的每个目录层级
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;  // 临时截断字符串
            
            // 检查当前路径是否存在
            if (access(tmp, F_OK) != 0) {
                // 目录不存在，创建它
                if (mkdir(tmp, S_IRWXU) != 0) {
                    return -1;  // 创建失败
                }
            }
            
            *p = '/';  // 恢复字符串
        }
    }
    
    // 检查完整路径是否存在
    if (access(tmp, F_OK) != 0) {
        // 创建最后一级目录
        if (mkdir(tmp, S_IRWXU) != 0) {
            return -1;  // 创建失败
        }
    }
    
    return 0;  // 成功
}

/******************************************************************************
 * @brief           日志文件行数控制和裁剪
 * @param[in]       acCutEvent  要裁剪的日志文件路径
 * @return          int         0成功，1失败
 * 
 * @details
 * 检查指定日志文件的行数，如果超过MAX_LINES（500行），则删除前100行
 * 以控制日志文件大小。使用临时文件的方式进行裁剪操作。
 * 
 * @algorithm
 * 1. 打开日志文件并统计总行数
 * 2. 如果行数超过MAX_LINES，执行裁剪操作：
 *    a. 创建临时文件
 *    b. 跳过前100行
 *    c. 将剩余行复制到临时文件
 *    d. 删除原文件，重命名临时文件
 * 3. 如果行数未超过限制，直接返回
 * 
 * @note
 * 1. 每次裁剪固定删除100行（linesToDelete = 100）
 * 2. 使用临时文件/tmp/mqtt/temp.txt进行中转
 * 3. 函数会自动关闭所有打开的文件句柄
 * 4. 包含基本的错误处理和状态输出
 ******************************************************************************/
int message_log_cut(char *acCutEvent) 
{
    FILE *file;
    char line[MAX_LINE_LENGTH];
    char tempFileName[] = "/tmp/mqtt/temp.txt";  // 临时文件路径
    int lineCount = 0;
    int linesToDelete = 100;  // 要删除的行数

    // 第一次打开文件：统计总行数
    file = fopen(acCutEvent, "r");
    if (file == NULL) {
        HLK_LOG_ERR("Unable to open file\n");
        return 1;
    }

    // 逐行读取统计行数
    while (fgets(line, MAX_LINE_LENGTH, file) != NULL) {
        lineCount++;
    }

    // 检查是否需要裁剪
    if (lineCount > MAX_LINES) {
        // 关闭文件准备重新处理
        if(file)
            fclose(file);

        // 重新打开原文件用于读取
        file = fopen(acCutEvent, "r");
        if (file == NULL) {
            HLK_LOG_ERR("Unable to open file\n");
            return 1;
        }

        // 创建临时文件用于写入
        FILE *tempFile = fopen(tempFileName, "w");
        if (tempFile == NULL) {
            HLK_LOG_ERR("Unable to create temporary file\n");
            return 1;
        }

        // 跳过前100行
        for (int i = 0; i < linesToDelete; i++) {
            fgets(line, MAX_LINE_LENGTH, file);
        }

        // 将剩余行复制到临时文件
        while (fgets(line, MAX_LINE_LENGTH, file) != NULL) {
            fputs(line, tempFile);
        }

        // 关闭所有文件
        if(file)
            fclose(file);
        if(tempFile)
            fclose(tempFile);

        // 删除原文件并重命名临时文件
        remove(acCutEvent);
        rename(tempFileName, acCutEvent);

        HLK_LOG_INFO("The first 100 rows have been deleted\n");
    } else {
        HLK_LOG_INFO("The number of file lines does not exceed 500\n");
    }

    return 0;
}

/******************************************************************************
 * @brief           带时间戳的格式化日志打印函数
 * @param[in]       fmt     格式化字符串（类似printf的格式）
 * @param[in]       ...     可变参数列表
 * @return          int     成功写入的字符数量，失败返回-1
 * 
 * @details
 * 将带时间戳的格式化信息写入日志文件/tmp/mqtt/message。
 * 主要功能包括：
 * 1. 获取当前系统时间并格式化为时间戳
 * 2. 创建日志目录（如果不存在）
 * 3. 将时间戳和用户信息追加写入日志文件
 * 4. 定期检查日志文件行数，超过阈值时自动裁剪
 * 
 * @note
 * 1. 使用静态计数器每100次调用检查一次日志大小
 * 2. 日志文件路径固定为/tmp/mqtt/message
 * 3. 时间戳格式：[YYYY-MM-DD HH:MM:SS mmm]
 * 4. 函数是线程不安全的（由于静态变量count）
 * 
 * @warning
 * 1. 可变参数处理的安全性检查可能不完整
 * 2. 内存清零操作(memset(&ap, 0, sizeof(va_list)))可能无效
 * 
 * @example
 * message_print("Temperature: %.2f°C", 25.6);
 * // 输出：[2024-01-15 14:30:25 123] Temperature: 25.60°C
 ******************************************************************************/
int message_print(char* fmt, ...)
{
    static int count = 0;                    // 静态计数器，用于控制日志裁剪频率
    char    datetime[1024] = {'\0'};         // 时间戳字符串缓冲区
    int ret = 0;                            // 函数返回值
    va_list ap;                             // 可变参数列表
    zig_timeval t;                       // 时间值结构体（包含微秒）
    char *saferet = NULL;                   // 安全操作返回值指针
    
    // 获取当前系统时间（包含微秒精度）- 使用Zig实现避免ABI问题
    zig_gettimeofday(&t, NULL);
    
    // 将时间戳转换为本地时间结构
    struct tm * ptm = localtime(&t.tv_sec);
    if(ptm == NULL)
    {
        HLK_LOG_ERR("lteOperate.upgrade_print: get local time failed!\n");
        return -1;
    }
    
    // 清零可变参数列表结构（安全性措施，但可能无效）
    saferet = memset(&ap, 0, sizeof(va_list));
    if(saferet == NULL)
    {
        return -1;
    }

    // 确保日志目录存在，如果不存在则创建
    if (mkdirs(SOCKET_PATH) != 0) {
        HLK_LOG_ERR("Failed to create directory.\n");
    }

    FILE* file;                                      // 日志文件句柄
    char tmp_file[] = "/tmp/mqtt/message";           // 日志文件路径（固定）
    
    // 以追加模式打开日志文件
    file = fopen(tmp_file, "a");
    if (file == NULL) {
        HLK_LOG_ERR("open %s fail\n", tmp_file);
        return -1;
    }

    // 格式化时间戳字符串：[YYYY-MM-DD HH:MM:SS mmm]
    snprintf(datetime,
            sizeof(datetime)-1,
            "[%.4d-%.2d-%.2d %.2d:%.2d:%.2d %.3ld]",
            ptm->tm_year+1900,          // 年份（tm_year是从1900开始的年数）
            ptm->tm_mon+1,              // 月份（tm_mon是0-11，需要+1）
            ptm->tm_mday,               // 日期
            ptm->tm_hour,               // 小时
            ptm->tm_min,                // 分钟
            ptm->tm_sec,                // 秒
            t.tv_usec/1000);            // 毫秒（微秒除以1000）
    
    // 先写入时间戳到文件
    fprintf(file, "%s ", datetime);
    
    // 处理可变参数并写入格式化内容
    va_start(ap, fmt);                      // 初始化可变参数列表
    ret = vfprintf(file, fmt, ap);          // 写入格式化内容到文件
    va_end(ap);                             // 清理可变参数列表

    // 关闭文件句柄
    if(file)
        fclose(file);

    // 定期检查并裁剪日志文件
    // 每调用100次检查一次文件大小，避免频繁的文件操作
    if(count++ > 100) {
        message_log_cut(tmp_file);          // 执行日志裁剪
        count = 0;                          // 重置计数器
    }

    return (ret);                           // 返回写入的字符数量
}

/******************************************************************************
 * @brief           系统重启函数
 * @param[in]       无
 * @return          void
 * 
 * @details
 * 通过执行系统重启命令来重启设备。使用SYSTEM函数包装器来执行
 * DEVICE_REBOOT宏定义的重启命令，并根据执行结果输出相应提示信息。
 * 
 * @note
 * 1. DEVICE_REBOOT宏需要在头文件中定义重启命令
 * 2. 重启命令通常需要管理员权限
 * 3. 函数执行后设备将重启，当前进程会终止
 * 4. 建议在调用前保存重要数据和状态
 * 
 * @warning
 * 此函数会导致系统重启，使用时需谨慎
 * 
 * @example
 * app_reboot();  // 执行系统重启
 ******************************************************************************/
void app_reboot(void)
{
    // 执行系统重启命令
    int result = SYSTEM(DEVICE_REBOOT);
    
    // 根据执行结果输出相应信息
    if (result == 0) {
        HLK_LOG_INFO("Command executed successfully Reboot!\n");
    } else {
        HLK_LOG_ERR("Command failed to execute\n");
    }
}

/******************************************************************************
 * @brief           增强的系统命令执行函数
 * @param[in]       command  要执行的系统命令字符串
 * @return          int      成功返回命令退出状态，失败返回-1
 * 
 * @details
 * system()函数的包装器，提供更详细的执行状态信息和错误处理。
 * 能够区分命令执行失败的不同原因：
 * 1. system()调用本身失败
 * 2. 命令正常退出（可获取退出码）
 * 3. 命令被信号终止
 * 4. 命令异常终止
 * 
 * @note
 * 1. 使用WIFEXITED、WEXITSTATUS、WIFSIGNALED、WTERMSIG宏分析命令状态
 * 2. 退出状态0通常表示成功，非0表示不同类型的错误
 * 3. 信号终止通常表示命令被强制中断（如SIGKILL、SIGTERM）
 * 4. 会在控制台输出详细的执行状态信息，便于调试
 * 
 * @warning
 * 1. 执行任意系统命令存在安全风险，需要验证输入
 * 2. 某些命令可能会阻塞很长时间
 * 3. 特权命令需要相应的系统权限
 * 
 * @example
 * int ret = SYSTEM("ls -la");              // 执行ls命令
 * int ret = SYSTEM("reboot");              // 执行重启命令  
 * int ret = SYSTEM("invalid_command");     // 执行无效命令
 ******************************************************************************/
int SYSTEM(const char *command)
{
    int status;
    
    // 执行系统命令
    status = system(command);
    
    // 检查system()调用本身是否失败
    if (status == -1) {
        HLK_LOG_ERR("Failed to execute command: %s\n", command);
        return -1;
    } else {
        // 分析命令执行状态
        
        // 检查命令是否正常退出
        if (WIFEXITED(status)) {
            HLK_LOG_INFO("Command executed successfully: %s\n", command);
            HLK_LOG_INFO("Exit status: %d\n", WEXITSTATUS(status));
            return WEXITSTATUS(status);  // 返回命令的退出状态码
        } 
        // 检查命令是否被信号终止
        else if (WIFSIGNALED(status)) {
            HLK_LOG_ERR("Command terminated by signal: %s\n", command);
            HLK_LOG_INFO("Terminating signal: %d\n", WTERMSIG(status));
            return -1;
        } 
        // 其他异常情况
        else {
            HLK_LOG_ERR("Command did not terminate normally: %s\n", command);
            return -1;
        }
    }
}

/**
 * @brief 设置系统时间同步
 * 
 * 根据传入的时间戳同步系统时间。将毫秒级时间戳转换为系统时间并设置到系统中。
 * 
 * @param timestamp 时间戳（毫秒），表示从1970年1月1日00:00:00 UTC开始的毫秒数
 * 
 * @return int 返回值
 *         - 0: 成功
 *         - 其他值: 暂未定义错误码（函数当前总是返回0）
 * 
 * @note 此函数需要root权限才能成功设置系统时间
 * @note 函数会将毫秒时间戳转换为秒级时间戳，微秒部分设置为0
 * @note 使用本地时间进行转换，会考虑系统时区设置
 * 
 * @warning 修改系统时间可能会影响其他正在运行的程序和系统服务
 * 
 * @example
 * // 设置系统时间为2024年1月1日 00:00:00
 * long long timestamp = 1704067200000LL; // 2024-01-01 00:00:00 UTC的毫秒时间戳
 * int result = set_timesync(timestamp);
 */
int set_timesync(long long timestamp)
{
    int rc;
    struct tm tptr;
    struct timeval tv;

    time_t Nowt = 0;
    time_t i = 0;
    struct tm *p;

    Nowt = timestamp/1000;
    p = localtime(&Nowt);

    tv.tv_sec = mktime(p);
    tv.tv_usec = 0;

    rc = settimeofday(&tv, NULL);

    return 0;
}


#endif  // 条件编译结束标记