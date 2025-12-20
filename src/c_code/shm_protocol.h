#ifndef SHM_PROTOCOL_H
#define SHM_PROTOCOL_H

#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>

// 宏定义
#define SHM_NAME "/edge_collector_shm"
#define EVENT_RING_SIZE 65536  // 2^16，利用位运算优化取模
#define MAX_POINTS 1000
#define MAX_DEVICES 32
#define NAME_LEN 16

// 数据类型枚举
typedef enum {
    SHM_TYPE_BOOL = 0,
    SHM_TYPE_UINT16 = 1,
    SHM_TYPE_INT16 = 2,
    SHM_TYPE_INT32 = 3,
    SHM_TYPE_FLOAT = 4
} ShmDataType;

// ShmValue 联合体 - 4字节对齐，紧凑存储各种数据类型
typedef union __attribute__((aligned(4))) {
    int32_t i32;
    uint32_t u32;
    float f32;
    uint16_t u16;
    int16_t i16;
    uint8_t b8;
    uint8_t raw[4];  // 用于字节级访问
} ShmValue;

// ShmPoint 静态元数据表 - 存储在全局数组 points[1000] 中
typedef struct __attribute__((aligned(8))) {
    char dev_name[NAME_LEN];     // 设备名称，16字节
    char pt_name[NAME_LEN];      // 点位名称，16字节
    uint8_t type;                // ShmDataType
    ShmValue cur_value;          // 当前值快照
    uint64_t update_ts;          // 最后更新时间戳(ms)
} ShmPoint;

// ShmEvent 动态事件单元 - 24字节设计（紧凑优化）
typedef struct __attribute__((aligned(8))) {
    uint64_t seq_id;             // 全局唯一递增序列号
    uint64_t timestamp_ms;       // 毫秒级时间戳
    uint16_t pt_index;           // 指向 points 数组的 Foreign Key
    ShmValue val;                // 变化后的值
    uint8_t padding[2];          // 填充至24字节对齐
} ShmEvent;

// ShmHeader 共享内存头部
typedef struct __attribute__((aligned(64))) {  // 缓存行对齐
    pthread_mutex_t mutex;        // 同步互斥锁
    pthread_cond_t cond;          // 条件变量，用于广播
    uint64_t head_seq;            // 当前最新的序列号
    uint32_t ring_mask;           // 环形缓冲区掩码 (EVENT_RING_SIZE - 1)
    uint32_t padding1;            // 填充对齐
    ShmEvent event_ring[EVENT_RING_SIZE];  // 环形事件队列
    ShmPoint points[MAX_POINTS];  // 全局点位池
} ShmHeader;

// 函数声明
int shm_init_master(ShmHeader **shm_ptr, size_t *shm_size);
int shm_init_slave(ShmHeader **shm_ptr, size_t *shm_size);
int shm_cleanup(ShmHeader *shm);
int shm_push_change(ShmHeader *shm, uint16_t pt_index, ShmValue new_value);
int shm_read_history(ShmHeader *shm, uint64_t *local_seq,
                     ShmEvent *out_buf, int max_count);

// 辅助函数
uint16_t shm_get_ring_index(uint64_t seq, uint32_t mask);
int shm_get_point_index_by_name(ShmHeader *shm, const char *dev_name, const char *pt_name);
int shm_set_point_info(ShmHeader *shm, uint16_t index,
                       const char *dev_name, const char *pt_name, uint8_t type);

#endif /* SHM_PROTOCOL_H */
