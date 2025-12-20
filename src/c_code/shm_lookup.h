#ifndef SHM_LOOKUP_H
#define SHM_LOOKUP_H

#include "shm_protocol.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

// Hash Map 实现思路 - Reader进程本地快速查找优化

// Hash Map 节点
typedef struct HashNode {
    char key[NAME_LEN * 2 + 1];  // "DeviceName.PointName" 格式
    uint16_t index;              // 对应的点位索引
    struct HashNode *next;       // 哈希冲突链表
} HashNode;

// Hash Map 结构
typedef struct {
    HashNode **buckets;          // 哈希桶数组
    size_t bucket_count;         // 桶数量
    size_t size;                 // 当前元素数量
} PointLookupMap;

// 函数声明
PointLookupMap* lookup_map_create(size_t bucket_count);
void lookup_map_destroy(PointLookupMap *map);
int lookup_map_insert(PointLookupMap *map, const char *dev_name, const char *pt_name, uint16_t index);
uint16_t lookup_map_get(PointLookupMap *map, const char *dev_name, const char *pt_name);
int lookup_map_build_from_shm(PointLookupMap *map, ShmHeader *shm);

// Hash函数 - djb2算法的简单变体
static inline size_t hash_string(const char *str) {
    size_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

// 组合键生成："DeviceName.PointName"
static inline void make_key(char *key_buf, size_t buf_size,
                           const char *dev_name, const char *pt_name) {
    snprintf(key_buf, buf_size, "%s.%s", dev_name, pt_name);
}

#endif /* SHM_LOOKUP_H */
