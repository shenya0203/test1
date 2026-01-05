#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h> // 新增：用于目录遍历
#include "cJSON.h"
#include "app_api.h"
#include "data_collector.h"

// 共享内存定义保持不变...
#define MODBUS_SHM_NAME "/modbus_shm"
#define MODBUS_MAX_INDEX_ENTRIES 100*50 

typedef struct {
    char device_name[50];
    char point_name[50];
    int absolute_index;
} modbus_index_entry_t;

typedef struct {
    int device_count;
    int total_points;
    int index_entry_count;
    int version;
    int last_update_time;
    modbus_index_entry_t index_table[MODBUS_MAX_INDEX_ENTRIES]; 
    double data[];
} modbus_shm_t;

// --- 辅助函数 ---

// 检查文件名后缀是否为 .json
static int is_json_file(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return 0;
    return strcmp(dot, ".json") == 0;
}

// 添加点位到全局列表
static void add_target_point(collector_ctx_t *ctx, const char *dev, const char *pt) 
{
    // 简单的去重检查（可选，如果配置保证不重复可去掉以提高性能）
    /*
    for(int i=0; i<ctx->target_count; i++) {
        if(strcmp(ctx->targets[i].device_name, dev) == 0 && 
           strcmp(ctx->targets[i].point_name, pt) == 0) return;
    }
    */

    if (ctx->target_count >= ctx->target_capacity) {
        int new_cap = ctx->target_capacity == 0 ? 64 : ctx->target_capacity * 2;
        target_point_t *new_arr = realloc(ctx->targets, new_cap * sizeof(target_point_t));
        if (!new_arr) return;
        ctx->targets = new_arr;
        ctx->target_capacity = new_cap;
    }
    
    target_point_t *t = &ctx->targets[ctx->target_count];
    strncpy(t->device_name, dev, sizeof(t->device_name) - 1);
    strncpy(t->point_name, pt, sizeof(t->point_name) - 1);
    t->shm_absolute_index = -1;
    t->current_value = 0.0;
    
    ctx->target_count++;
}

// 解析单个文件的 ucld_node 节点
static void parse_cloud_content(collector_ctx_t *ctx, cJSON *root, const char *filename)
{
    printf("[Collector] 开始解析Cloud内容: 文件 %s\n", filename);

    cJSON *ucld_node = cJSON_GetObjectItem(root, "ucld_node");
    if (!ucld_node) {
        printf("[Collector] 文件 %s 中未找到 ucld_node 字段\n", filename);
        return;
    }

    if (!cJSON_IsArray(ucld_node)) {
        printf("[Collector] 文件 %s 中的 ucld_node 不是数组类型\n", filename);
        return;
    }

    int count = cJSON_GetArraySize(ucld_node);
    printf("[Collector] 文件 %s 包含 %d 个设备配置\n", filename, count);

    int added = 0;
    int skipped_devices = 0;
    int skipped_points = 0;

    for (int i = 0; i < count; i++) {
        cJSON *device = cJSON_GetArrayItem(ucld_node, i);
        if (!device) {
            printf("[Collector] 文件 %s 第 %d 个设备配置无效\n", filename, i + 1);
            continue;
        }

        cJSON *slave_name = cJSON_GetObjectItem(device, "slave_name");
        cJSON *node_list = cJSON_GetObjectItem(device, "node_list");

        if (!cJSON_IsString(slave_name)) {
            printf("[Collector] 文件 %s 第 %d 个设备缺少有效的 slave_name\n", filename, i + 1);
            skipped_devices++;
            continue;
        }

        if (!cJSON_IsArray(node_list)) {
            printf("[Collector] 文件 %s 设备 %s 的 node_list 不是数组类型\n", filename, slave_name->valuestring);
            skipped_devices++;
            continue;
        }

        const char *dev_name = slave_name->valuestring;
        int pt_count = cJSON_GetArraySize(node_list);
        printf("[Collector] 处理设备 %s，包含 %d 个点位\n", dev_name, pt_count);

        int device_added = 0;
        for (int j = 0; j < pt_count; j++) {
            cJSON *pt = cJSON_GetArrayItem(node_list, j);
            if (!cJSON_IsString(pt)) {
                printf("[Collector] 设备 %s 的第 %d 个点位不是字符串类型\n", dev_name, j + 1);
                skipped_points++;
                continue;
            }

            printf("[Collector] 添加点位: 设备=%s, 点位=%s\n", dev_name, pt->valuestring);
            add_target_point(ctx, dev_name, pt->valuestring);
            added++;
            device_added++;
        }

        if (device_added > 0) {
            printf("[Collector] 设备 %s 添加了 %d 个点位\n", dev_name, device_added);
        }
    }

    printf("[Collector] 文件 %s 解析完成: 总共添加 %d 个点位，跳过 %d 个设备，%d 个点位\n",
           filename, added, skipped_devices, skipped_points);
}

// 处理单个配置文件
static void process_config_file(collector_ctx_t *ctx, const char *full_path, const char *filename)
{
    printf("[Collector] 开始处理配置文件: %s (路径: %s)\n", filename, full_path);

    FILE *fp = fopen(full_path, "r");
    if (!fp) {
        fprintf(stderr, "[Collector] 无法打开配置文件 %s: %s\n", filename, strerror(errno));
        return;
    }
    printf("[Collector] 成功打开配置文件 %s\n", filename);

    // 获取文件大小
    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "[Collector] 无法获取文件大小 %s: %s\n", filename, strerror(errno));
        fclose(fp);
        return;
    }

    long len = ftell(fp);
    if (len < 0) {
        fprintf(stderr, "[Collector] 获取文件大小失败 %s: %s\n", filename, strerror(errno));
        fclose(fp);
        return;
    }

    printf("[Collector] 配置文件 %s 大小: %ld 字节\n", filename, len);

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[Collector] 无法重置文件指针 %s: %s\n", filename, strerror(errno));
        fclose(fp);
        return;
    }

    // 分配内存
    char *data = malloc(len + 1);
    if (!data) {
        fprintf(stderr, "[Collector] 内存分配失败 %s: 需要 %ld 字节\n", filename, len + 1);
        fclose(fp);
        return;
    }

    // 读取文件内容
    size_t read_bytes = fread(data, 1, len, fp);
    if (read_bytes != (size_t)len) {
        free(data);
        fclose(fp);
        return;
    }
    data[len] = '\0';
    fclose(fp);

    // 解析JSON
    cJSON *json = cJSON_Parse(data);
    free(data);

    if (!json) {
        return;
    }

    // 检查link字段
    cJSON *link = cJSON_GetObjectItem(json, "link");
    if (!link) {
        cJSON_Delete(json);
        return;
    }

    if (!cJSON_IsString(link)) {
        cJSON_Delete(json);
        return;
    }

    // 关键逻辑：只处理 Cloud 模式
    if (strcmp(link->valuestring, "Cloud") == 0) {
        printf("[Collector] 检测到Cloud模式，开始解析点位配置 %s\n", filename);
        parse_cloud_content(ctx, json, filename);
    } else {
        printf("[Collector] 跳过非Cloud模式配置文件 %s (模式: %s)\n", filename, link->valuestring);
    }
    //解析其他参数

    cJSON_Delete(json);
}

// 映射共享内存索引
static int map_shm(collector_ctx_t *ctx) 
{
    printf("[Collector] 调用 Zig 接口映射共享内存...\n");
    
    ZigShmResult result;
    // 显式指定完整路径，这比依赖 shm_open 的环境配置更可靠
    const char *shm_path = "/dev/shm/modbus_shm";
    
    // 调用 Zig 实现
    zig_map_modbus_shm(shm_path, &result);

    if (!result.success) {
        printf("[Collector] Zig 映射失败!\n");
        return -1;
    }

    // 填充到 C 的上下文中
    ctx->shm_fd = result.fd;
    ctx->shm_ptr = result.ptr;
    ctx->shm_size = result.size;

    printf("[Collector] Zig 映射成功! Addr=%p, Size=%zu\n", ctx->shm_ptr, ctx->shm_size);

    // 这里的逻辑保持不变：遍历所有收集到的点位，去 SHM 索引表中查找下标
    modbus_shm_t *shm = (modbus_shm_t *)ctx->shm_ptr;
    int matched = 0;

    for (int i = 0; i < ctx->target_count; i++) {
        target_point_t *target = &ctx->targets[i];
        
        for (int k = 0; k < shm->index_entry_count; k++) {
            modbus_index_entry_t *entry = &shm->index_table[k];
            if (strcmp(target->device_name, entry->device_name) == 0 &&
                strcmp(target->point_name, entry->point_name) == 0) {
                target->shm_absolute_index = entry->absolute_index;
                matched++;
                break;
            }
        }
    }
    printf("[Collector] Total mapped points: %d/%d\n", matched, ctx->target_count);
    return 0;
}

// --- API 实现 ---

collector_ctx_t* collector_init(void) 
{
    char *config_dir = "/etc/config/device/edge_report/";
    printf("[Collector] Scanning config dir: %s\n", config_dir);
    
    collector_ctx_t *ctx = calloc(1, sizeof(collector_ctx_t));
    if (!ctx) return NULL;
    ctx->shm_fd = -1;

    DIR *dir = opendir(config_dir);
    if (!dir) {
        perror("[Collector] Cannot open config directory");
        free(ctx);
        return NULL;
    }

    struct dirent *ent;
    char full_path[512];

    // 1. 遍历目录，加载所有 Cloud 配置
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG || ent->d_type == DT_UNKNOWN) { // 常规文件
            if (is_json_file(ent->d_name)) {
                snprintf(full_path, sizeof(full_path), "%s/%s", config_dir, ent->d_name);
                process_config_file(ctx, full_path, ent->d_name);
            }
        }
    }
    closedir(dir);

    if (ctx->target_count == 0) {
        printf("[Collector] No Cloud points found in any config files.\n");
        return ctx;
    }

    // 2. 统一映射共享内存
    //if (map_shm(ctx) != 0) {
        // 即使 SHM 失败，ctx 仍然有效，只是采不到数据
    //}

    return ctx;
}

// 内部函数：尝试重连共享内存
static int try_remap_shm(collector_ctx_t *ctx) 
{
    // 1. 先清理旧的（如果有）
    if (ctx->shm_ptr && ctx->shm_ptr != MAP_FAILED) {
        printf("[Collector] 清理旧的映射...\n");
        zig_unmap_modbus_shm(ctx->shm_ptr, ctx->shm_size, ctx->shm_fd);
        ctx->shm_ptr = NULL;
        ctx->shm_fd = -1;
        ctx->is_connected = 0;
    }

    // 2. 尝试建立新映射
    // 使用之前定义的 map_shm (它内部调用 Zig)
    // 注意：这里需要稍微修改一下 map_shm，让它不要重复做点位匹配，或者接受重连标志
    // 为了简单，我们这里直接调用 map_shm，它会更新 fd 和 ptr
    
    printf("[Collector] 尝试重新建立映射...\n");
    if (map_shm(ctx) == 0) {
        printf("[Collector] 重连成功！\n");
        ctx->is_connected = 1;
        ctx->last_change_time = zig_get_timestamp();
        
        // 读取当前心跳作为初始值
        modbus_shm_t *shm = (modbus_shm_t *)ctx->shm_ptr;
        return 0;
    }
    
    return -1;
}

void collector_sync_data(collector_ctx_t *ctx) 
{
    if (!ctx) return;

    const char *shm_path = "/dev/shm/modbus_shm";
    time_t now = zig_get_timestamp();
    

    // ==========================================
    // 阶段 1: 连接状态检查与恢复
    // ==========================================
    if (ctx->is_connected == 0) {
        // 如果当前未连接，尝试连接
        if (try_remap_shm(ctx) != 0) {
            // 连接失败（可能采集进程还没启动），直接返回，下次再试
            // 可以打印个日志，但不要刷屏
            // printf("[Collector] 等待采集进程启动...\n"); 
            return; 
        }
    }

    // ==========================================
    // 阶段 2: 文件一致性检查 (Inode Check)
    // ==========================================
    // 检查文件是否被删除重建了
    if (!zig_check_shm_inode(ctx->shm_fd, shm_path)) {
        printf("[Collector] 警告：共享内存文件失效 (Inode 变更或文件丢失)，触发重连...\n");
        ctx->is_connected = 0; // 标记为断开
        return; // 本次跳过，下次循环会进入阶段1重连
    }

    modbus_shm_t *shm = (modbus_shm_t *)ctx->shm_ptr;
    
    // 心跳没变：检查是否超时 (例如超过 5 秒没更新)
    if (zig_time_diff_abs(now, shm->last_update_time) > 5) {
        printf("[Collector] 警告：共享内存数据僵死 (心跳未更新 > 5s)，采集进程可能已挂起。\n");
        printf("[Collector] now: %lld, shm->last_update_time: %lld\n", now, shm->last_update_time);
    }

    // ==========================================
    // 阶段 4: 正常数据同步
    // ==========================================
    // 只有连接正常才读取数据
    // ... 原有的读取逻辑 ...
    for (int i = 0; i < ctx->target_count; i++) {
        if (ctx->targets[i].shm_absolute_index >= 0) {
            ctx->targets[i].current_value = shm->data[ctx->targets[i].shm_absolute_index];
            printf("[Collector] %s.%s = %f\n", ctx->targets[i].device_name, ctx->targets[i].point_name, ctx->targets[i].current_value);
        }
    }
}

void collector_destroy(collector_ctx_t *ctx) 
{
    if (!ctx) return;
    
    // 使用 Zig 的清理函数
    if (ctx->shm_ptr && ctx->shm_ptr != MAP_FAILED) {
        zig_unmap_modbus_shm(ctx->shm_ptr, ctx->shm_size, ctx->shm_fd);
    }
    
    if (ctx->targets) free(ctx->targets);
    free(ctx);
}
