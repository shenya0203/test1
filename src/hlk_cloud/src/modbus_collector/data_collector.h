#ifndef DATA_COLLECTOR_H
#define DATA_COLLECTOR_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>


// ... 结构体定义保持不变 ...
typedef struct {
    char device_name[50];
    char point_name[50];
    int  shm_absolute_index; 
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

int collector_report_data(collector_ctx_t *ctx);

#endif