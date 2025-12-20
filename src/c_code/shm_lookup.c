#include "shm_lookup.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 创建Hash Map
PointLookupMap* lookup_map_create(size_t bucket_count) {
    PointLookupMap *map = (PointLookupMap *)malloc(sizeof(PointLookupMap));
    if (!map) return NULL;

    map->buckets = (HashNode **)calloc(bucket_count, sizeof(HashNode *));
    if (!map->buckets) {
        free(map);
        return NULL;
    }

    map->bucket_count = bucket_count;
    map->size = 0;

    return map;
}

// 销毁Hash Map
void lookup_map_destroy(PointLookupMap *map) {
    if (!map) return;

    for (size_t i = 0; i < map->bucket_count; i++) {
        HashNode *node = map->buckets[i];
        while (node) {
            HashNode *next = node->next;
            free(node);
            node = next;
        }
    }

    free(map->buckets);
    free(map);
}

// 插入键值对
int lookup_map_insert(PointLookupMap *map, const char *dev_name, const char *pt_name, uint16_t index) {
    if (!map || !dev_name || !pt_name) return -1;

    // 生成组合键
    char key[NAME_LEN * 2 + 1];
    make_key(key, sizeof(key), dev_name, pt_name);

    // 计算哈希值
    size_t hash_val = hash_string(key);
    size_t bucket_idx = hash_val % map->bucket_count;

    // 检查是否已存在
    HashNode *node = map->buckets[bucket_idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            // 更新现有节点
            node->index = index;
            return 0;
        }
        node = node->next;
    }

    // 创建新节点
    HashNode *new_node = (HashNode *)malloc(sizeof(HashNode));
    if (!new_node) return -1;

    strncpy(new_node->key, key, sizeof(new_node->key) - 1);
    new_node->key[sizeof(new_node->key) - 1] = '\0';
    new_node->index = index;
    new_node->next = map->buckets[bucket_idx];

    map->buckets[bucket_idx] = new_node;
    map->size++;

    return 0;
}

// 查找索引 - O(1) 复杂度
uint16_t lookup_map_get(PointLookupMap *map, const char *dev_name, const char *pt_name) {
    if (!map || !dev_name || !pt_name) return (uint16_t)-1;

    // 生成组合键
    char key[NAME_LEN * 2 + 1];
    make_key(key, sizeof(key), dev_name, pt_name);

    // 计算哈希值
    size_t hash_val = hash_string(key);
    size_t bucket_idx = hash_val % map->bucket_count;

    // 在链表中查找
    HashNode *node = map->buckets[bucket_idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            return node->index;
        }
        node = node->next;
    }

    return (uint16_t)-1; // 未找到
}

// 从共享内存构建Hash Map - Reader进程启动时调用
int lookup_map_build_from_shm(PointLookupMap *map, ShmHeader *shm) {
    if (!map || !shm) return -1;

    int count = 0;

    // 遍历所有点位，构建Hash Map
    for (uint16_t i = 0; i < MAX_POINTS; i++) {
        if (shm->points[i].dev_name[0] != '\0') {
            // 有效的点位
            if (lookup_map_insert(map, shm->points[i].dev_name,
                                 shm->points[i].pt_name, i) == 0) {
                count++;
            } else {
                fprintf(stderr, "Failed to insert point %s.%s into lookup map\n",
                        shm->points[i].dev_name, shm->points[i].pt_name);
            }
        }
    }

    printf("Built lookup map with %d points\n", count);
    return count;
}

// 使用示例代码（添加到Reader进程的初始化中）：
//
// // Reader进程初始化
// ShmHeader *shm = NULL;
// size_t shm_size;
// if (shm_init_slave(&shm, &shm_size) != 0) {
//     fprintf(stderr, "Failed to init slave SHM\n");
//     return -1;
// }
//
// // 创建本地Hash Map用于快速查找
// PointLookupMap *lookup_map = lookup_map_create(1024); // 1024个桶
// if (!lookup_map) {
//     fprintf(stderr, "Failed to create lookup map\n");
//     return -1;
// }
//
// // 从共享内存构建Hash Map
// if (lookup_map_build_from_shm(lookup_map, shm) < 0) {
//     fprintf(stderr, "Failed to build lookup map from SHM\n");
//     return -1;
// }
//
// // Reader主循环
// uint64_t local_seq = 0; // 初始化为0，开始读取最新事件
// ShmEvent events[100];   // 批量读取缓冲区
//
// while (running) {
//     int count = shm_read_history(shm, &local_seq, events, 100);
//     if (count > 0) {
//         for (int i = 0; i < count; i++) {
//             ShmEvent *event = &events[i];
//             uint16_t pt_idx = event->pt_index;
//
//             // 通过索引快速访问点位信息（避免字符串拷贝）
//             ShmPoint *point = &shm->points[pt_idx];
//             printf("Event: %s.%s = ", point->dev_name, point->pt_name);
//
//             // 根据数据类型显示值
//             switch (point->type) {
//                 case SHM_TYPE_BOOL:
//                     printf("%d", event->val.b8 ? 1 : 0);
//                     break;
//                 case SHM_TYPE_UINT16:
//                     printf("%u", event->val.u16);
//                     break;
//                 case SHM_TYPE_INT16:
//                     printf("%d", event->val.i16);
//                     break;
//                 case SHM_TYPE_INT32:
//                     printf("%d", event->val.i32);
//                     break;
//                 case SHM_TYPE_FLOAT:
//                     printf("%.3f", event->val.f32);
//                     break;
//             }
//             printf(" at %llu ms\n", (unsigned long long)event->timestamp_ms);
//         }
//     } else if (count == -2) {
//         printf("Reader overrun detected, resetting sequence\n");
//     }
//
//     // 可以添加条件变量等待或定时轮询
//     usleep(10000); // 10ms轮询
// }
