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


// 修改：传入配置目录路径，扫描该目录下所有 JSON
collector_ctx_t* collector_init(void);

// 下面接口保持不变
void collector_sync_data(collector_ctx_t *ctx);
void collector_destroy(collector_ctx_t *ctx);

#endif