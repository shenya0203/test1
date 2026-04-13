#ifndef DATA_COLLECTOR_H
#define DATA_COLLECTOR_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>

// 数据类型枚举
typedef enum {
    //BIT = 1,           // 位数据（别名）
    UNSIGNED_16 = 1,       // 无符号整数（别名）
    SIGNED_16 = 2,         // 有符号整数（别名）
    BCD_16 = 3,             //16位 BCD
    INT32_ABCD = 4,          // 32位有符号整数（AB CD）- Big Endian
    INT32_CDAB = 5,     // 32位有符号整数（CD AB）- Little Endian
    INT32_DCBA = 6,     // 32位 有符号(DC BA)
    UINT32_ABCD = 7,         // 32位无符号整数（AB CD）- Big Endian
    UINT32_CDAB = 8,    // 32位无符号整数（CD AB）- Little Endian
    UINT32_DCBA = 9,    // 32位 无符号(DC BA)
    FLOAT_ABCD = 10,    // 浮点数（AB CD）- Big Endian
    FLOAT_CDAB = 11,    // 浮点数（CD AB）- Little Endian
    FLOAT_DCBA = 12,    // 32位 浮点数(DC BA)
    BCD_32     = 13,    // 32位 BCD
    FLOAT_64_ABCD = 14, // 64位 浮点数(AB CD)
    FLOAT_64_CDAB = 15, // 64位 浮点数(CD AB)
    TM_32 = 16,         // 32位 时间戳
    BTYE_30 = 17,        // 30字节 定位型
    BIT = 18,           // 布尔值
    STRING = 12,        // 字符串
} value_type_t;



// ... 结构体定义保持不变 ...
typedef struct {
    char device_name[50];
    char point_name[50];
    int  shm_absolute_index; 
    unsigned char decimal_places;            // 小数位数
    unsigned char value_type;                // 数值类型
    double current_value;
} target_point_t;

// 定时上报配置结构体
typedef struct {
    int type;  // 0:关闭, 1:整时, 2:整刻, 3:整分, 4:固定时间
    int hh;    // 小时
    int mm;    // 分钟
} timed_report_config_t;

// 上报条件配置结构体
typedef struct {
    int period;                    // 周期上报 (0:关闭, 非0:秒数)
    timed_report_config_t timed;   // 定时上报配置
} report_condition_t;

// Cloud上报配置结构体
typedef struct {
    int change_report_type;       // 变化上报类型 (0:关闭, 1:开启)
    char name[64];                // 配置名称
    int data_report_type;         // 数据格式 (0:源类型, 1:转为字符串)
    int err_enable;               // 是否开启错误填充 (0:关闭, 1:开启)
    char err_info[256];           // 自定义错误信息
    report_condition_t cond;      // 上报条件
} cloud_report_config_t;

typedef struct {
    int shm_fd;
    void *shm_ptr;
    size_t shm_size;

    // 新增：用于健康检查
    uint32_t last_heartbeat;
    time_t last_change_time;
    int is_connected; // 0: 断开, 1: 连接

    target_point_t *targets;
    int target_count;
    int target_capacity;

    // 新增：Cloud上报配置
    cloud_report_config_t report_config;
} collector_ctx_t;

// 定义与 Zig 对应的结构体
typedef struct {
    int fd;
    void *ptr;
    size_t size;
    char success;
} ZigShmResult;

// 声明外部 Zig 函数
extern void zig_map_modbus_shm(const char *path, ZigShmResult *out_result);
extern void zig_unmap_modbus_shm(void *ptr, size_t size, int fd);
extern char zig_check_shm_inode(int fd, const char *path);

// 声明时间相关函数
extern struct tm *zig_get_localtime(const time_t *timer);


// 修改：传入配置目录路径，扫描该目录下所有 JSON
collector_ctx_t* collector_init(void);

// 下面接口保持不变
void collector_sync_data(collector_ctx_t *ctx);

// 新增：独立的上报接口，对接私有云接口
int collector_report_data(collector_ctx_t *ctx);

void collector_destroy(collector_ctx_t *ctx);

#endif