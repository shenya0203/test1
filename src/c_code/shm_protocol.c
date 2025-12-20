#include "shm_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef __MUSL__
// 在musl环境下避免包含时间头文件，但允许zig的musl提供必要的定义
// time_t和NULL已由zig的musl头文件定义
#else
#include <time.h>
#include <sys/time.h>
#endif

// 定义兼容的时间函数，避免64位函数问题
static uint64_t get_current_timestamp_ms(void) {
#ifdef __MUSL__
    // 在musl环境下返回固定时间戳以完全避免时间函数调用
    return 1000000000ULL; // 固定的时间戳
#else
    time_t now = time(NULL);
    return (uint64_t)now * 1000;
#endif
}

// 初始化共享内存 - Master (Writer) 模式
int shm_init_master(ShmHeader **shm_ptr, size_t *shm_size) {
    int fd;
    (void)shm_size; // Mark as unused for now

    // 计算共享内存大小
    *shm_size = sizeof(ShmHeader);

    // 创建共享内存对象
    fd = shm_open(SHM_NAME, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd == -1) {
        if (errno == EEXIST) {
            // 如果已存在，删除后重新创建
            shm_unlink(SHM_NAME);
            fd = shm_open(SHM_NAME, O_CREAT | O_RDWR | O_EXCL, 0666);
            if (fd == -1) {
                perror("shm_open after unlink");
                return -1;
            }
        } else {
            perror("shm_open");
            return -1;
        }
    }

    // 设置共享内存大小
    if (ftruncate(fd, *shm_size) == -1) {
        perror("ftruncate");
        goto cleanup_fd;
    }

    // 映射共享内存
    *shm_ptr = mmap(NULL, *shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (*shm_ptr == MAP_FAILED) {
        perror("mmap");
        goto cleanup_fd;
    }

    // 初始化共享内存头部
    memset(*shm_ptr, 0, sizeof(ShmHeader));

    // 初始化同步原语
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;

    if (pthread_mutexattr_init(&mutex_attr) != 0) {
        perror("pthread_mutexattr_init");
        goto cleanup_mmap;
    }

    // 设置互斥锁为进程间共享
    if (pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_mutexattr_setpshared");
        pthread_mutexattr_destroy(&mutex_attr);
        goto cleanup_mmap;
    }

    if (pthread_mutex_init(&(*shm_ptr)->mutex, &mutex_attr) != 0) {
        perror("pthread_mutex_init");
        pthread_mutexattr_destroy(&mutex_attr);
        goto cleanup_mmap;
    }

    if (pthread_condattr_init(&cond_attr) != 0) {
        perror("pthread_condattr_init");
        pthread_mutexattr_destroy(&mutex_attr);
        goto cleanup_mutex;
    }

    // 设置条件变量为进程间共享
    if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_condattr_setpshared");
        pthread_condattr_destroy(&cond_attr);
        goto cleanup_mutex;
    }

    if (pthread_cond_init(&(*shm_ptr)->cond, &cond_attr) != 0) {
        perror("pthread_cond_init");
        pthread_condattr_destroy(&cond_attr);
        goto cleanup_mutex;
    }

    // 初始化其他字段
    (*shm_ptr)->head_seq = 0;
    (*shm_ptr)->ring_mask = EVENT_RING_SIZE - 1;

    // 初始化点位数组（清空）
    memset((*shm_ptr)->points, 0, sizeof((*shm_ptr)->points));

    pthread_mutexattr_destroy(&mutex_attr);
    pthread_condattr_destroy(&cond_attr);
    close(fd);

    return 0;

cleanup_mutex:
    pthread_mutex_destroy(&(*shm_ptr)->mutex);
cleanup_mmap:
    munmap(*shm_ptr, *shm_size);
cleanup_fd:
    close(fd);
    shm_unlink(SHM_NAME);
    return -1;
}

// 初始化共享内存 - Slave (Reader) 模式
int shm_init_slave(ShmHeader **shm_ptr, size_t *shm_size) {
    int fd;

    // 计算共享内存大小
    *shm_size = sizeof(ShmHeader);

    // 打开已存在的共享内存对象
    fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open (slave)");
        return -1;
    }

#ifdef __MUSL__
    // 在musl环境下跳过fstat检查以避免64位时间函数问题
    // 假设共享内存大小是正确的
    printf("Warning: Skipping shared memory size verification on musl\n");
#else
    // 获取共享内存大小
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return -1;
    }

    if ((size_t)st.st_size != *shm_size) {
        fprintf(stderr, "Shared memory size mismatch: expected %zu, got %lld\n",
                *shm_size, (long long)st.st_size);
        close(fd);
        return -1;
    }
#endif

    // 映射共享内存
    *shm_ptr = mmap(NULL, *shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (*shm_ptr == MAP_FAILED) {
        perror("mmap (slave)");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

// 清理共享内存资源
int shm_cleanup(ShmHeader *shm) {
    if (!shm) return 0;

    // 销毁同步原语
    pthread_mutex_destroy(&shm->mutex);
    pthread_cond_destroy(&shm->cond);

    // 取消映射
    munmap(shm, sizeof(ShmHeader));

    // 删除共享内存对象（仅Master调用）
    shm_unlink(SHM_NAME);

    return 0;
}

// 快速环形缓冲区索引计算
uint16_t shm_get_ring_index(uint64_t seq, uint32_t mask) {
    return (uint16_t)(seq & mask);
}

// 推入数据变化事件 (Writer调用)
int shm_push_change(ShmHeader *shm, uint16_t pt_index, ShmValue new_value) {
    if (!shm || pt_index >= MAX_POINTS) {
        return -1;
    }

    pthread_mutex_lock(&shm->mutex);

    // 检查点位索引有效性
    if (shm->points[pt_index].dev_name[0] == '\0') {
        pthread_mutex_unlock(&shm->mutex);
        return -1; // 点位未初始化
    }

    uint64_t timestamp = get_current_timestamp_ms();
    uint64_t seq_id = ++shm->head_seq;

    // 更新点位当前值
    shm->points[pt_index].cur_value = new_value;
    shm->points[pt_index].update_ts = timestamp;

    // 创建事件并写入环形缓冲区
    uint16_t ring_idx = shm_get_ring_index(seq_id, shm->ring_mask);
    ShmEvent *event = &shm->event_ring[ring_idx];

    event->seq_id = seq_id;
    event->timestamp_ms = timestamp;
    event->pt_index = pt_index;
    event->val = new_value;
    memset(event->padding, 0, sizeof(event->padding));

    // 广播唤醒所有Reader
    pthread_cond_broadcast(&shm->cond);

    pthread_mutex_unlock(&shm->mutex);

    return 0;
}

// 读取历史事件 (Reader调用)
int shm_read_history(ShmHeader *shm, uint64_t *local_seq,
                     ShmEvent *out_buf, int max_count) {
    if (!shm || !local_seq || !out_buf || max_count <= 0) {
        return -1;
    }

    int ret = 0;

    pthread_mutex_lock(&shm->mutex);

    uint64_t head_seq = shm->head_seq;

    // 检查是否有新数据
    if (head_seq <= *local_seq) {
        pthread_mutex_unlock(&shm->mutex);
        return 0; // 无新数据
    }

    // 回绕检测：如果Reader严重滞后，数据已被覆盖
    uint64_t seq_diff = head_seq - *local_seq;
    if (seq_diff > EVENT_RING_SIZE) {
        // Reader滞后严重，重置到最新位置（接受部分数据丢失）
        fprintf(stderr, "Reader overrun detected: head_seq=%llu, local_seq=%llu, diff=%llu\n",
                (unsigned long long)head_seq, (unsigned long long)*local_seq,
                (unsigned long long)seq_diff);
        *local_seq = head_seq;
        pthread_mutex_unlock(&shm->mutex);
        return -2; // 回绕错误码
    }

    // 计算需要读取的事件数量
    int read_count = (int)seq_diff;
    if (read_count > max_count) {
        read_count = max_count;
    }

    // 处理环形缓冲区的边界情况
    uint16_t start_idx = shm_get_ring_index(*local_seq + 1, shm->ring_mask);
    uint16_t end_idx = shm_get_ring_index(*local_seq + read_count, shm->ring_mask);

    if (start_idx <= end_idx) {
        // 数据连续，直接复制
        memcpy(out_buf, &shm->event_ring[start_idx],
               read_count * sizeof(ShmEvent));
    } else {
        // 数据跨越环形缓冲区边界，需要分两段复制
        uint32_t first_part = EVENT_RING_SIZE - start_idx;
        uint32_t second_part = read_count - first_part;

        // 第一段：从start_idx到数组尾部
        memcpy(out_buf, &shm->event_ring[start_idx],
               first_part * sizeof(ShmEvent));

        // 第二段：从数组头部开始
        memcpy(out_buf + first_part, &shm->event_ring[0],
               second_part * sizeof(ShmEvent));
    }

    // 更新本地序列号
    *local_seq += read_count;
    ret = read_count;

    pthread_mutex_unlock(&shm->mutex);

    return ret;
}

// 设置点位信息
int shm_set_point_info(ShmHeader *shm, uint16_t index,
                       const char *dev_name, const char *pt_name, uint8_t type) {
    if (!shm) {
        fprintf(stderr, "shm_set_point_info: shm pointer is NULL\n");
        return -1;
    }

    if (index >= MAX_POINTS) {
        fprintf(stderr, "shm_set_point_info: index %u exceeds MAX_POINTS %d\n",
                index, MAX_POINTS);
        return -1;
    }

    if (!dev_name) {
        fprintf(stderr, "shm_set_point_info: dev_name is NULL\n");
        return -1;
    }

    if (!pt_name) {
        fprintf(stderr, "shm_set_point_info: pt_name is NULL\n");
        return -1;
    }

    size_t dev_name_len = strlen(dev_name);
    size_t pt_name_len = strlen(pt_name);

    if (dev_name_len >= NAME_LEN) {
        fprintf(stderr, "shm_set_point_info: dev_name '%s' length %zu exceeds NAME_LEN %d\n",
                dev_name, dev_name_len, NAME_LEN);
        return -1; // 名称过长
    }

    if (pt_name_len >= NAME_LEN) {
        fprintf(stderr, "shm_set_point_info: pt_name '%s' length %zu exceeds NAME_LEN %d\n",
                pt_name, pt_name_len, NAME_LEN);
        return -1; // 名称过长
    }

    pthread_mutex_lock(&shm->mutex);

    // 检查是否已被使用
    if (shm->points[index].dev_name[0] != '\0') {
        fprintf(stderr, "shm_set_point_info: point index %u already in use by '%s.%s'\n",
                index, shm->points[index].dev_name, shm->points[index].pt_name);
        pthread_mutex_unlock(&shm->mutex);
        return -1; // 索引已被占用
    }

    // 设置点位信息
    strncpy(shm->points[index].dev_name, dev_name, NAME_LEN - 1);
    shm->points[index].dev_name[NAME_LEN - 1] = '\0';

    strncpy(shm->points[index].pt_name, pt_name, NAME_LEN - 1);
    shm->points[index].pt_name[NAME_LEN - 1] = '\0';

    shm->points[index].type = type;
    shm->points[index].update_ts = 0;

    // 初始化当前值为0
    memset(&shm->points[index].cur_value, 0, sizeof(ShmValue));

    pthread_mutex_unlock(&shm->mutex);

    return 0;
}

// 通过名称查找点位索引
int shm_get_point_index_by_name(ShmHeader *shm, const char *dev_name, const char *pt_name) {
    if (!shm || !dev_name || !pt_name) {
        return -1;
    }

    // 注意：这个函数是线性的O(n)查找，在Reader进程中应该使用Hash Map优化
    for (uint16_t i = 0; i < MAX_POINTS; i++) {
        if (strcmp(shm->points[i].dev_name, dev_name) == 0 &&
            strcmp(shm->points[i].pt_name, pt_name) == 0) {
            return i;
        }
    }

    return -1; // 未找到
}
