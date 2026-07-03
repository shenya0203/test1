#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h> // 新增：用于目录遍历
#include <time.h>   // 新增：用于时间戳
#include <sys/types.h> // 新增：用于 mkdir
#include <signal.h>
#include "cJSON.h"
#include "app_api.h"
#include "data_collector.h"
#include "hlk_log.h"
#include "hi_mqtt.h"
#include <libubus.h>
#include <libubox/blobmsg_json.h>

// 全局上下文指针，供MQTT回调和UBUS使用
collector_ctx_t *g_collector_ctx = NULL;
static volatile sig_atomic_t g_signal_report_pending = 0;

static void collector_signal_report_handler(int signo)
{
    if (signo == SIGUSR1) {
        g_signal_report_pending = 1;
    }
}

// 共享内存定义保持不变...
#define MODBUS_SHM_NAME "/modbus_shm"
#define MODBUS_MAX_INDEX_ENTRIES 100*50 

typedef struct {
    char device_name[50];
    char point_name[50];
    int absolute_index;
    unsigned char decimal_places;            // 小数位数
    unsigned char value_type;                // 数值类型
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

// --- 缓存管理宏定义 ---
#define CACHE_DIR "/mnt/data/modbus_cache/"
#define MAX_CACHE_FILE_SIZE (8 * 1024) // 8KB
#define MAX_TOTAL_CACHE_SIZE (1024 * 1024) // 1MB

// --- 辅助函数 ---

// 检查文件名后缀是否为 .json
static int is_json_file(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return 0;
    return strcmp(dot, ".json") == 0;
}

// 检查文件名是否为缓存文件 (cloud_*.cache)
static int is_cache_file(const char *filename) {
    if (!filename || strlen(filename) < 11) return 0; // "cloud_.cache" 最短长度
    return strncmp(filename, "cloud_", 6) == 0 && strstr(filename, ".cache") != NULL;
}

// 获取缓存文件名中的时间戳 (从 "cloud_<timestamp>.cache" 中提取 timestamp)
static time_t get_cache_file_timestamp(const char *filename) {
    if (!is_cache_file(filename)) return 0;

    const char *start = filename + 6; // 跳过 "cloud_"
    const char *end = strstr(start, ".cache");
    if (!end) return 0;

    char timestamp_str[32] = {0};
    size_t len = end - start;
    if (len >= sizeof(timestamp_str)) return 0;

    strncpy(timestamp_str, start, len);
    timestamp_str[len] = '\0';

    return (time_t)atol(timestamp_str);
}

// 获取缓存目录下的所有缓存文件列表，按时间戳排序（最旧的在前）
static char** get_cache_files_list(int *count) {
    if (!count) return NULL;
    *count = 0;

    DIR *dir = opendir(CACHE_DIR);
    if (!dir) {
        // 目录不存在，尝试创建
        mkdir(CACHE_DIR, 0755);
        return NULL;
    }

    struct dirent *ent;
    char **files = NULL;
    int capacity = 0;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG && is_cache_file(ent->d_name)) {
            if (*count >= capacity) {
                capacity = capacity == 0 ? 16 : capacity * 2;
                char **new_files = realloc(files, capacity * sizeof(char*));
                if (!new_files) {
                    closedir(dir);
                    if (files) free(files);
                    return NULL;
                }
                files = new_files;
            }

            files[*count] = strdup(ent->d_name);
            if (!files[*count]) {
                closedir(dir);
                for (int i = 0; i < *count; i++) free(files[i]);
                free(files);
                return NULL;
            }
            (*count)++;
        }
    }
    closedir(dir);

    if (*count == 0) {
        free(files);
        return NULL;
    }

    // 按时间戳排序（冒泡排序，最旧的在前）
    for (int i = 0; i < *count - 1; i++) {
        for (int j = 0; j < *count - i - 1; j++) {
            time_t ts1 = get_cache_file_timestamp(files[j]);
            time_t ts2 = get_cache_file_timestamp(files[j + 1]);
            if (ts1 > ts2) {
                char *temp = files[j];
                files[j] = files[j + 1];
                files[j + 1] = temp;
            }
        }
    }

    return files;
}

// 清理文件列表内存
static void free_cache_files_list(char **files, int count) {
    if (!files) return;
    for (int i = 0; i < count; i++) {
        if (files[i]) free(files[i]);
    }
    free(files);
}

// 检查并清理磁盘空间，确保总缓存大小不超过限制
static int check_and_clean_disk_space(void) {
    DIR *dir = opendir(CACHE_DIR);
    if (!dir) {
        mkdir(CACHE_DIR, 0755);
        return 0;
    }

    size_t total_size = 0;
    struct dirent *ent;

    // 计算总大小
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG && is_cache_file(ent->d_name)) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s%s", CACHE_DIR, ent->d_name);

            struct stat st;
            if (stat(full_path, &st) == 0) {
                total_size += st.st_size;
            }
        }
    }
    closedir(dir);

    // 如果总大小超过限制，删除最旧的文件
    while (total_size > MAX_TOTAL_CACHE_SIZE) {
        int file_count;
        char **files = get_cache_files_list(&file_count);
        if (!files || file_count == 0) {
            free_cache_files_list(files, file_count);
            break;
        }

        // 删除最旧的文件
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s%s", CACHE_DIR, files[0]);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            total_size -= st.st_size;
        }

        if (unlink(full_path) == 0) {
            HLK_LOG_INFO("[Cache] Deleted old cache file: %s\n", files[0]);
        }

        free_cache_files_list(files, file_count);
    }

    return 0;
}

// 查找最旧的缓存文件
static char* find_oldest_cache_file(void) {
    int file_count;
    char **files = get_cache_files_list(&file_count);
    if (!files || file_count == 0) {
        free_cache_files_list(files, file_count);
        return NULL;
    }

    char *oldest_file = files[0];
    files[0] = NULL; // 防止被释放

    free_cache_files_list(files, file_count);
    return oldest_file;
}

// 写入离线缓存数据
static int write_offline_cache(const char *data) {
    if (!data) {
        HLK_LOG_INFO("[Cache] Error: invalid data to cache\n");
        return -1;
    }

    // 检查并清理磁盘空间
    if (check_and_clean_disk_space() != 0) {
        HLK_LOG_INFO("[Cache] Error: failed to clean disk space\n");
        return -1;
    }

    // 查找最新的缓存文件
    char latest_file[256] = {0};
    time_t latest_timestamp = 0;

    DIR *dir = opendir(CACHE_DIR);
    if (!dir) {
        mkdir(CACHE_DIR, 0755);
        dir = opendir(CACHE_DIR);
        if (!dir) {
            HLK_LOG_ERR("[Cache] Error: cannot open cache directory\n");
            return -1;
        }
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type == DT_REG && is_cache_file(ent->d_name)) {
            time_t ts = get_cache_file_timestamp(ent->d_name);
            if (ts > latest_timestamp) {
                latest_timestamp = ts;
                strncpy(latest_file, ent->d_name, sizeof(latest_file) - 1);
            }
        }
    }
    closedir(dir);

    char full_path[512];
    int need_new_file = 1;

    if (latest_file[0] != '\0') {
        // 检查现有文件大小
        snprintf(full_path, sizeof(full_path), "%s%s", CACHE_DIR, latest_file);
        struct stat st;
        if (stat(full_path, &st) == 0 && st.st_size < MAX_CACHE_FILE_SIZE) {
            need_new_file = 0;
        }
    }

    if (need_new_file) {
        // 创建新文件
        time_t now = zig_get_timestamp();
        snprintf(latest_file, sizeof(latest_file), "cloud_%lld.cache", now);
        snprintf(full_path, sizeof(full_path), "%s%s", CACHE_DIR, latest_file);
    }

    // 追加写入数据（以换行符结尾）
    FILE *fp = fopen(full_path, "a");
    if (!fp) {
        HLK_LOG_ERR("[Cache] Error: cannot open cache file %s for writing\n", full_path);
        return -1;
    }

    fprintf(fp, "%s\n", data);
    fclose(fp);

    HLK_LOG_INFO("[Cache] Successfully cached data to %s\n", latest_file);
    return 0;
}

// 处理一个缓存文件（逐行发送，发送成功后删除文件）
static int flush_one_cache_file(void) {
    char *oldest_file = find_oldest_cache_file();
    if (!oldest_file) {
        return 0; // 没有缓存文件
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", CACHE_DIR, oldest_file);

    FILE *fp = fopen(full_path, "r");
    if (!fp) {
        HLK_LOG_ERR("[Cache] Error: cannot open cache file %s for reading\n", full_path);
        free(oldest_file);
        return -1;
    }

    HLK_LOG_INFO("[Cache] Processing cache file: %s\n", oldest_file);

    char *line = NULL;
    size_t len = 0;
    int success_count = 0;
    int fail_count = 0;

    // 创建临时文件来保存未发送成功的数据
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.temp", full_path);
    FILE *temp_fp = NULL;

    while (getline(&line, &len, fp) != -1) {
        // 移除行末的换行符
        size_t line_len = strlen(line);
        if (line_len > 0 && line[line_len - 1] == '\n') {
            line[line_len - 1] = '\0';
            line_len--;
        }
        if (line_len == 0) continue; // 跳过空行

        // 发送数据
        int rc = hlk_mqtt_publish(mqtt_topic_type_table[DATA_POINTS_UP].topic, QOS0, line, strlen(line));
        if (rc == 0) {
            success_count++;
            HLK_LOG_INFO("[Cache] Successfully sent cached data: %s\n", line);
        } else {
            fail_count++;
            HLK_LOG_ERR("[Cache] Failed to send cached data: %s\n", line);

            // 如果还没有创建临时文件，现在创建
            if (!temp_fp) {
                temp_fp = fopen(temp_path, "w");
                if (!temp_fp) {
                    HLK_LOG_ERR("[Cache] Error: cannot create temp file %s\n", temp_path);
                    break;
                }
            }

            // 将失败的数据写入临时文件
            fprintf(temp_fp, "%s\n", line);
        }
    }

    free(line);
    fclose(fp);

    if (temp_fp) {
        fclose(temp_fp);

        // 如果有未发送的数据，替换原文件
        if (fail_count > 0) {
            if (rename(temp_path, full_path) != 0) {
                HLK_LOG_ERR("[Cache] Error: cannot rename temp file to %s\n", full_path);
                unlink(temp_path); // 删除临时文件
            } else {
                HLK_LOG_INFO("[Cache] Kept %d failed records in cache file\n", fail_count);
            }
        } else {
            unlink(temp_path); // 删除空的临时文件
        }
    }

    // 如果全部发送成功，删除缓存文件
    if (fail_count == 0 && success_count > 0) {
        if (unlink(full_path) == 0) {
            HLK_LOG_INFO("[Cache] Successfully deleted processed cache file: %s (%d records sent)\n",
                   oldest_file, success_count);
        } else {
            HLK_LOG_ERR("[Cache] Error: cannot delete cache file %s\n", full_path);
        }
    } else if (fail_count > 0) {
        HLK_LOG_INFO("[Cache] Partially processed cache file: %s (%d sent, %d failed)\n",
               oldest_file, success_count, fail_count);
    }

    free(oldest_file);
    return fail_count == 0 ? 0 : -1; // 返回0表示完全成功，-1表示部分失败
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
    HLK_LOG_INFO("[Collector] 开始解析Cloud内容: 文件 %s\n", filename);

    cJSON *ucld_node = cJSON_GetObjectItem(root, "ucld_node");
    if (!ucld_node) {
        HLK_LOG_ERR("[Collector] 文件 %s 中未找到 ucld_node 字段\n", filename);
        return;
    }

    if (!cJSON_IsArray(ucld_node)) {
        HLK_LOG_ERR("[Collector] 文件 %s 中的 ucld_node 不是数组类型\n", filename);
        return;
    }

    int count = cJSON_GetArraySize(ucld_node);
    HLK_LOG_INFO("[Collector] 文件 %s 包含 %d 个设备配置\n", filename, count);

    int added = 0;
    int skipped_devices = 0;
    int skipped_points = 0;

    for (int i = 0; i < count; i++) {
        cJSON *device = cJSON_GetArrayItem(ucld_node, i);
        if (!device) {
            HLK_LOG_ERR("[Collector] 文件 %s 第 %d 个设备配置无效\n", filename, i + 1);
            continue;
        }

        cJSON *slave_name = cJSON_GetObjectItem(device, "slave_name");
        cJSON *node_list = cJSON_GetObjectItem(device, "node_list");

        if (!cJSON_IsString(slave_name)) {
            HLK_LOG_ERR("[Collector] 文件 %s 第 %d 个设备缺少有效的 slave_name\n", filename, i + 1);
            skipped_devices++;
            continue;
        }

        if (!cJSON_IsArray(node_list)) {
            HLK_LOG_ERR("[Collector] 文件 %s 设备 %s 的 node_list 不是数组类型\n", filename, slave_name->valuestring);
            skipped_devices++;
            continue;
        }

        const char *dev_name = slave_name->valuestring;
        int pt_count = cJSON_GetArraySize(node_list);
        HLK_LOG_INFO("[Collector] 处理设备 %s，包含 %d 个点位\n", dev_name, pt_count);

        int device_added = 0;
        for (int j = 0; j < pt_count; j++) {
            cJSON *pt = cJSON_GetArrayItem(node_list, j);
            if (!cJSON_IsString(pt)) {
                HLK_LOG_ERR("[Collector] 设备 %s 的第 %d 个点位不是字符串类型\n", dev_name, j + 1);
                skipped_points++;
                continue;
            }

            //HLK_LOG_DEBUG("[Collector] 添加点位: 设备=%s, 点位=%s\n", dev_name, pt->valuestring);
            add_target_point(ctx, dev_name, pt->valuestring);
            added++;
            device_added++;
        }

        if (device_added > 0) {
            HLK_LOG_INFO("[Collector] 设备 %s 添加了 %d 个点位\n", dev_name, device_added);
        }
    }

    HLK_LOG_INFO("[Collector] 文件 %s 解析完成: 总共添加 %d 个点位，跳过 %d 个设备，%d 个点位\n",
           filename, added, skipped_devices, skipped_points);
}

// 解析Cloud上报配置参数
static void parse_cloud_report_config(collector_ctx_t *ctx, cJSON *root, const char *filename)
{
    HLK_LOG_INFO("[Collector] 开始解析Cloud上报配置参数: 文件 %s\n", filename);

    cloud_report_config_t *config = &ctx->report_config;

    // 解析change_report_type字段
    cJSON *change_report_type = cJSON_GetObjectItem(root, "change_report_type");
    if (change_report_type && cJSON_IsNumber(change_report_type)) {
        config->change_report_type = change_report_type->valueint;
        HLK_LOG_INFO("[Collector] change_report_type: %d\n", config->change_report_type);
    }

    // 解析name字段
    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (name && cJSON_IsString(name)) {
        strncpy(config->name, name->valuestring, sizeof(config->name) - 1);
        HLK_LOG_INFO("[Collector] name: %s\n", config->name);
    }

    // 解析data_report_type字段
    cJSON *data_report_type = cJSON_GetObjectItem(root, "data_report_type");
    if (data_report_type && cJSON_IsNumber(data_report_type)) {
        config->data_report_type = data_report_type->valueint;
        HLK_LOG_INFO("[Collector] data_report_type: %d\n", config->data_report_type);
    }

    // 解析err_enable字段
    cJSON *err_enable = cJSON_GetObjectItem(root, "err_enable");
    if (err_enable && cJSON_IsNumber(err_enable)) {
        config->err_enable = err_enable->valueint;
        HLK_LOG_INFO("[Collector] err_enable: %d\n", config->err_enable);
    }

    // 解析err_info字段
    cJSON *err_info = cJSON_GetObjectItem(root, "err_info");
    if (err_info && cJSON_IsString(err_info)) {
        strncpy(config->err_info, err_info->valuestring, sizeof(config->err_info) - 1);
        HLK_LOG_INFO("[Collector] err_info: %s\n", config->err_info);
    }

    // 解析cond字段 (上报条件)
    cJSON *cond = cJSON_GetObjectItem(root, "cond");
    if (cond && cJSON_IsObject(cond)) {
        // 解析period字段
        cJSON *period = cJSON_GetObjectItem(cond, "period");
        if (period && cJSON_IsNumber(period)) {
            config->cond.period = period->valueint;
            HLK_LOG_INFO("[Collector] cond.period: %d\n", config->cond.period);
        }

        // 解析timed字段
        cJSON *timed = cJSON_GetObjectItem(cond, "timed");
        if (timed && cJSON_IsObject(timed)) {
            cJSON *type = cJSON_GetObjectItem(timed, "type");
            if (type && cJSON_IsNumber(type)) {
                config->cond.timed.type = type->valueint;
                HLK_LOG_INFO("[Collector] cond.timed.type: %d\n", config->cond.timed.type);
            }

            cJSON *hh = cJSON_GetObjectItem(timed, "hh");
            if (hh && cJSON_IsNumber(hh)) {
                config->cond.timed.hh = hh->valueint;
                HLK_LOG_INFO("[Collector] cond.timed.hh: %d\n", config->cond.timed.hh);
            }

            cJSON *mm = cJSON_GetObjectItem(timed, "mm");
            if (mm && cJSON_IsNumber(mm)) {
                config->cond.timed.mm = mm->valueint;
                HLK_LOG_INFO("[Collector] cond.timed.mm: %d\n", config->cond.timed.mm);
            }
        }
    }

    HLK_LOG_INFO("[Collector] Cloud上报配置参数解析完成: 文件 %s\n", filename);
}

// 处理单个配置文件
static void process_config_file(collector_ctx_t *ctx, const char *full_path, const char *filename)
{
    HLK_LOG_INFO("[Collector] 开始处理配置文件: %s (路径: %s)\n", filename, full_path);

    FILE *fp = fopen(full_path, "r");
    if (!fp) {
        fprintf(stderr, "[Collector] 无法打开配置文件 %s: %s\n", filename, strerror(errno));
        return;
    }
    HLK_LOG_INFO("[Collector] 成功打开配置文件 %s\n", filename);

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

    HLK_LOG_INFO("[Collector] 配置文件 %s 大小: %ld 字节\n", filename, len);

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
    if ((strcmp(link->valuestring, "Cloud") == 0) || (strcmp(link->valuestring, "CLOUD") == 0)) {
        HLK_LOG_INFO("[Collector] 检测到Cloud模式，开始解析点位配置 %s\n", filename);
        parse_cloud_content(ctx, json, filename);
        // 解析Cloud上报配置参数
        parse_cloud_report_config(ctx, json, filename);
    } else {
        HLK_LOG_INFO("[Collector] 跳过非Cloud模式配置文件 %s (模式: %s)\n", filename, link->valuestring);
    }
    

    cJSON_Delete(json);
}

// 映射共享内存索引
static int map_shm(collector_ctx_t *ctx) 
{
    HLK_LOG_INFO("[Collector] 调用 Zig 接口映射共享内存...\n");
    
    ZigShmResult result;
    // 显式指定完整路径，这比依赖 shm_open 的环境配置更可靠
    const char *shm_path = "/dev/shm/modbus_shm";
    
    // 调用 Zig 实现
    zig_map_modbus_shm(shm_path, &result);

    if (!result.success) {
        HLK_LOG_ERR("[Collector] Zig 映射失败!\n");
        return -1;
    }

    // 填充到 C 的上下文中
    ctx->shm_fd = result.fd;
    ctx->shm_ptr = result.ptr;
    ctx->shm_size = result.size;


    // 这里的逻辑保持不变：遍历所有收集到的点位，去 SHM 索引表中查找下标
    modbus_shm_t *shm = (modbus_shm_t *)ctx->shm_ptr;
    int matched = 0;
    HLK_LOG_INFO("[Collector] Zig 映射成功! Addr=%p, Size=%zu, index_entry_count=%d\n", ctx->shm_ptr, ctx->shm_size, shm->index_entry_count);

    {
        int iii;
        for (iii = 0; iii < ctx->target_count; iii++) {
            target_point_t *target = &ctx->targets[iii];
            HLK_LOG_INFO("[Collector] target[%d] = %s.%s\n", iii, target->device_name, target->point_name);
        }

        for (iii = 0; iii < shm->index_entry_count; iii++) {
            modbus_index_entry_t *entry = &shm->index_table[iii];
            HLK_LOG_INFO("[Collector] index_table[%d] = %s.%s, absolute_index=%d\n", iii, entry->device_name, entry->point_name, entry->absolute_index);
        }
    }

    for (int i = 0; i < ctx->target_count; i++) {
        target_point_t *target = &ctx->targets[i];
        
        for (int k = 0; k < shm->index_entry_count; k++) {
            modbus_index_entry_t *entry = &shm->index_table[k];
            if (strcmp(target->device_name, entry->device_name) == 0 &&
                strcmp(target->point_name, entry->point_name) == 0) {
                HLK_LOG_INFO("[Collector] 找到点位 %s.%s, absolute_index=%d\n", target->device_name, target->point_name, entry->absolute_index);
                target->shm_absolute_index = entry->absolute_index;
                target->decimal_places = entry->decimal_places;
                target->value_type = entry->value_type;
                matched++;
                break;
            }
        }
    }

    HLK_LOG_INFO("[Collector] Total mapped points: %d/%d\n", matched, ctx->target_count);

    return 0;
}

// --- API 实现 ---

collector_ctx_t* collector_init(void) 
{
    char *config_dir = "/etc/config/device/edge_report/";
    HLK_LOG_INFO("[Collector] Scanning config dir: %s\n", config_dir);
    
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
        HLK_LOG_ERR("[Collector] No Cloud points found in any config files.\n");
        return ctx;
    }

    // 2. 统一映射共享内存
    //if (map_shm(ctx) != 0) {
        // 即使 SHM 失败，ctx 仍然有效，只是采不到数据
    //}

    g_collector_ctx = ctx;
    return ctx;
}

// 内部函数：尝试重连共享内存
static int try_remap_shm(collector_ctx_t *ctx) 
{
    // 1. 先清理旧的（如果有）
    if (ctx->shm_ptr && ctx->shm_ptr != MAP_FAILED) {
        HLK_LOG_INFO("[Collector] 清理旧的映射...\n");
        zig_unmap_modbus_shm(ctx->shm_ptr, ctx->shm_size, ctx->shm_fd);
        ctx->shm_ptr = NULL;
        ctx->shm_fd = -1;
        ctx->is_connected = 0;
    }

    // 2. 尝试建立新映射
    // 使用之前定义的 map_shm (它内部调用 Zig)
    // 注意：这里需要稍微修改一下 map_shm，让它不要重复做点位匹配，或者接受重连标志
    // 为了简单，我们这里直接调用 map_shm，它会更新 fd 和 ptr
    
    HLK_LOG_INFO("[Collector] 尝试重新建立映射...\n");
    if (map_shm(ctx) == 0) {
        HLK_LOG_INFO("[Collector] 重连成功！\n");
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
    if (ctx->is_connected == 0) {   //是否连接
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
        HLK_LOG_WARN("[Collector] 警告：共享内存文件失效 (Inode 变更或文件丢失)，触发重连...\n");
        ctx->is_connected = 0; // 标记为断开
        return; // 本次跳过，下次循环会进入阶段1重连
    }

    modbus_shm_t *shm = (modbus_shm_t *)ctx->shm_ptr;
    
    // 心跳没变：检查是否超时 (例如超过 5 秒没更新)
    if (zig_time_diff_abs(now, shm->last_update_time) > 5) {
        HLK_LOG_WARN("[Collector] 警告：共享内存数据僵死 (心跳未更新 > 5s)，采集进程可能已挂起。\n");
        HLK_LOG_INFO("[Collector] now: %lld, shm->last_update_time: %d\n", now, shm->last_update_time);
    }

    // ==========================================
    // 阶段 4: 正常数据同步
    // ==========================================
    // 只有连接正常才读取数据
    // ... 原有的读取逻辑 ...
    for (int i = 0; i < ctx->target_count; i++) {
        if (ctx->targets[i].shm_absolute_index >= 0) {
            ctx->targets[i].current_value = shm->data[ctx->targets[i].shm_absolute_index];
            //HLK_LOG_DEBUG("[Collector] %s.%s = %f\n", ctx->targets[i].device_name, ctx->targets[i].point_name, ctx->targets[i].current_value);
        }
    }
}

// 检查是否需要上报（根据周期和定时配置）
static int should_report_data(collector_ctx_t *ctx)
{
    if (!ctx) return 0;

    cloud_report_config_t *config = &ctx->report_config;
    time_t now;
    struct tm *tm_now = NULL;

    now = zig_get_timestamp();
    tm_now = zig_get_localtime(&now);

    //printf("[Collector] tm_now->tm_hour: %d, tm_now->tm_min: %d, tm_now->tm_sec: %d\n", tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);

    // 检查周期上报
    if (config->cond.period > 0) {
        // 简单的周期检查：每period秒上报一次
        // 这里可以根据实际需要实现更复杂的逻辑，比如记录上次上报时间
        static time_t last_period_report = 0;
        if (now - last_period_report >= config->cond.period) {
            last_period_report = now;
            //HLK_LOG_INFO("[Collector] last_period_report: %lld\n", last_period_report);
            return 1;
        }
    }
    //HLK_LOG_INFO("[Collector] config->cond.period: %d\n", config->cond.period);

    // 检查定时上报
    if (config->cond.timed.type != 0) {
        switch (config->cond.timed.type) {
            case 1: // 整时
                if (tm_now->tm_min == 0 && tm_now->tm_sec == 0) {
                    return 1;
                }
                break;
            case 2: // 整刻（每15分钟）
                if (tm_now->tm_min % 15 == 0 && tm_now->tm_sec == 0) {
                    return 1;
                }
                break;
            case 3: // 整分
                if (tm_now->tm_sec == 0) {
                    return 1;
                }
                break;
            case 4: // 固定时间
                if (tm_now->tm_hour == config->cond.timed.hh &&
                    tm_now->tm_min == config->cond.timed.mm) {
                    return 1;
                }
                break;
            default:
                break;
        }
    }

    return 0;
}

// 准备上报数据（URL编码格式）
static char* prepare_report_data_urlencoded(collector_ctx_t *ctx)
{
    //HLK_LOG_DEBUG("[DATA_COLLECTOR] 开始准备上报数据\n");

    if (!ctx || !ctx->targets || ctx->target_count <= 0) {
        HLK_LOG_INFO("[DATA_COLLECTOR] 参数无效 - ctx=%p, targets=%p, count=%d\n",
               ctx, ctx ? ctx->targets : NULL, ctx ? ctx->target_count : 0);
        return NULL;
    }

    //HLK_LOG_DEBUG("[DATA_COLLECTOR] 配置名称=%s, 目标点位数量=%d\n",ctx->report_config.name, ctx->target_count);

    // 估算字符串长度：时间戳(25) + 每个点位的空间(设备名50 + 点位名50 + 数值25 + 分隔符5) * 点位数
    // 额外预留设备DN标签的空间
    size_t estimated_len = 25; // time=xxxxxxxxx&
    for (int i = 0; i < ctx->target_count; i++) {
        estimated_len += 50 + 50 + 25 + 5; // DN=设备名&点位名=数值&
    }

    //HLK_LOG_DEBUG("[DATA_COLLECTOR] 预估字符串长度=%zu\n", estimated_len);

    // 分配内存
    char *result = (char*)malloc(estimated_len);
    if (!result) {
        //HLK_LOG_INFO("[DATA_COLLECTOR] 内存分配失败\n");
        return NULL;
    }

    // 清空字符串
    result[0] = '\0';
    size_t current_len = 0;

    // 添加时间戳
    time_t now = zig_get_timestamp();
    char time_str[25];
    snprintf(time_str, sizeof(time_str), "time=%lld&", now);
    strncat(result, time_str, estimated_len - current_len - 1);
    current_len = strlen(result);
    //HLK_LOG_DEBUG("[DATA_COLLECTOR] 添加时间戳 time=%lld\n", now);

    // 用于跟踪当前设备
    char current_device[50] = "";

    // 遍历所有点位
    for (int i = 0; i < ctx->target_count; i++) {
        target_point_t *target = &ctx->targets[i];

        #if 0   //更新上报格式 去掉DeviceName 直接上报点位数据 (数据点名称唯一)
        // 检查是否是新设备
        if (strcmp(current_device, target->device_name) != 0) {
            // 添加设备标识符
            char dn_str[60]; // DN= + 设备名 + &
            snprintf(dn_str, sizeof(dn_str), "DN=%s&", target->device_name);
            strncat(result, dn_str, estimated_len - current_len - 1);
            current_len = strlen(result);

            // 更新当前设备
            strncpy(current_device, target->device_name, sizeof(current_device) - 1);
            current_device[sizeof(current_device) - 1] = '\0';

            HLK_LOG_INFO("[DATA_COLLECTOR] 切换到新设备 %s\n", target->device_name);
        }
        #endif

        // 添加点位数据（小数位数由 decimal_places 决定）
        char point_str[85]; // 点位名 + = + 数值 + &
        if (target->value_type == FLOAT_ABCD || target->value_type == FLOAT_CDAB || target->value_type == FLOAT_DCBA) {
            snprintf(point_str, sizeof(point_str), "%s=%.*f&", target->point_name, (int)target->decimal_places, target->current_value);
        } else {
            snprintf(point_str, sizeof(point_str), "%s=%.0f&", target->point_name, target->current_value);
        }
        strncat(result, point_str, estimated_len - current_len - 1);
        current_len = strlen(result);

        //HLK_LOG_DEBUG("[DATA_COLLECTOR] 添加点位 %s=%.*f\n", target->point_name, (int)target->decimal_places, target->current_value);
    }

    // 移除最后一个&符号
    if (current_len > 0 && result[current_len - 1] == '&') {
        result[current_len - 1] = '\0';
        current_len--;
    }

    //HLK_LOG_DEBUG("[DATA_COLLECTOR] 数据准备完成，最终长度=%zu\n", current_len);
    //HLK_LOG_DEBUG("[DATA_COLLECTOR] 结果: %s\n", result);

    // 构建JSON格式数据
    cJSON *root = cJSON_CreateObject();
    char *response_str = NULL;

    if (!root) {
        HLK_LOG_INFO("[DATA_COLLECTOR] 创建JSON根对象失败\n");
        free(result);
        return NULL;
    }

    // 添加DeviceCode字段
    cJSON_AddStringToObject(root, "DeviceCode", mqtt_user_cert.deviceName);

    // 创建Items数组
    cJSON *items_array = cJSON_CreateArray();
    if (!items_array) {
        HLK_LOG_INFO("[DATA_COLLECTOR] 创建Items数组失败\n");
        cJSON_Delete(root);
        free(result);
        return NULL;
    }

    // 创建单个Item对象
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        HLK_LOG_INFO("[DATA_COLLECTOR] 创建Item对象失败\n");
        cJSON_Delete(items_array);
        cJSON_Delete(root);
        free(result);
        return NULL;
    }

    // 添加Time字段（使用now时间戳）
    cJSON_AddNumberToObject(item, "Time", now);

    // 添加Name字段（固定为"DataPointsUp"）
    cJSON_AddStringToObject(item, "Name", "DataPointsUp");

    // 添加Value字段（使用完整的result字符串）
    cJSON_AddStringToObject(item, "Value", result);

    // 将item添加到数组
    cJSON_AddItemToArray(items_array, item);

    // 将Items数组添加到根对象
    cJSON_AddItemToObject(root, "Items", items_array);

    // 生成JSON字符串
    response_str = cJSON_PrintUnformatted(root);
    if (!response_str) {
        HLK_LOG_DEBUG("[DATA_COLLECTOR] 生成JSON字符串失败\n");
        cJSON_Delete(root);
        free(result);
        return NULL;
    }

    // 清理JSON对象
    cJSON_Delete(root);

    free(result);
    
    return response_str;
}

// 准备上报数据（JSON格式）- 保留原有函数用于兼容性
static char* prepare_report_data(collector_ctx_t *ctx)
{
    //HLK_LOG_INFO("[DATA_COLLECTOR] prepare_report_data: 开始准备上报数据\n");
    //上报数据格式
    /*
    上报数据格式：DN=DeviceName1&node0101=123&node2=xxx&node3=xxx&DN=xx&node1=xxx&node2=xxx
    time=xxxx&DN= DeviceName1 从设备名称：DeviceName1 两个DN中间是它的所有上报属性
    node0101=123  数据点名称：node0101 数据点值：123
    */

    return NULL;
}


static int collector_do_report_data(collector_ctx_t *ctx, const char *reason)
{
    if (!ctx) {
        HLK_LOG_ERR("[Collector] Report error: invalid context\n");
        return -1;
    }

    //HLK_LOG_DEBUG("[Collector] Start report, reason: %s\n", reason ? reason : "unknown");
    collector_sync_data(ctx);

    // ==========================================
    // 步骤 A：准备数据
    // ==========================================
    char *report_data = prepare_report_data_urlencoded(ctx);
    if (!report_data) {
        HLK_LOG_ERR("[Collector] Report error: failed to prepare data\n");
        return -1;
    }
    //HLK_LOG_DEBUG("[Collector] Report data: %s\n", report_data);

    int realtime_send_success = 0;

    // ==========================================
    // 步骤 B：尝试发送实时数据
    // ==========================================
    if (sharedData.connect_status == MQTT_CONNECT_STATUS_CONNECTED) {

        
        //HLK_LOG_INFO("[Collector] MQTT connected, attempting to send realtime data...\n");
        #if 1
        int rc = hlk_mqtt_publish(mqtt_topic_type_table[TOPIC_POST].topic, QOS0, report_data, strlen(report_data));
        if (rc == 0) {
            //HLK_LOG_DEBUG("[Collector] Realtime data sent successfully\n");
            realtime_send_success = 1;
        } else {
            HLK_LOG_ERR("[Collector] Failed to send realtime data (rc=%d)\n", rc);
        }
        #endif
    } else {
        //HLK_LOG_ERR("[Collector] MQTT not connected (status=%d), skipping realtime send\n", sharedData.connect_status);
    }

    // ==========================================
    // 步骤 C：处理缓存（仅在实时发送成功后执行）
    // ==========================================
    if (realtime_send_success) {
        //HLK_LOG_DEBUG("[Collector] Processing cache data...\n");
        //flush_one_cache_file();
    } else {
       //HLK_LOG_ERR("[Collector] Skipping cache processing (realtime send failed)\n");
    }

    #if 0
    // ==========================================
    // 步骤 D：保存离线数据
    // ==========================================
    if (!realtime_send_success) {
        printf("[Collector] Saving data to offline cache...\n");
        if (write_offline_cache(report_data) != 0) {
            printf("[Collector] Failed to save data to cache\n");
        }
    } else {
        printf("[Collector] Skipping offline cache (realtime send succeeded)\n");
    }
    #endif

    // 释放内存
    free(report_data);

    return realtime_send_success ? 0 : -1;
}

// 独立的Cloud数据上报接口
int collector_report_data(collector_ctx_t *ctx)
{
    if (!ctx) {
        HLK_LOG_ERR("[Collector] Report error: invalid context\n");
        return -1;
    }

    // 检查是否需要上报
    if (!should_report_data(ctx)) {
        //printf("[Collector] Skip report: conditions not met\n");
        return 1; // 1 表示 skip
    }
    //HLK_LOG_DEBUG("[Collector] should_report_data: 1\n");

    return collector_do_report_data(ctx, "period_or_timed");
}

int collector_report_signal_data(collector_ctx_t *ctx)
{
    return collector_do_report_data(ctx, "SIGUSR1");
}

int collector_register_signal_report_handler(void)
{
    if (signal(SIGUSR1, collector_signal_report_handler) == SIG_ERR) {
        HLK_LOG_ERR("[Collector] Failed to register SIGUSR1 handler: %s\n", strerror(errno));
        return -1;
    }

    //HLK_LOG_DEBUG("[Collector] SIGUSR1 report handler registered\n");
    return 0;
}

int collector_take_signal_report_pending(void)
{
    if (g_signal_report_pending) {
        g_signal_report_pending = 0;
        return 1;
    }

    return 0;
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

// UBUS调用完成的回调函数
static void ubus_invoke_callback(struct ubus_request *req, int type, struct blob_attr *msg) {
    (void)req;
    (void)type;
    (void)msg;
}

// UBUS调用，设置指定设备的数据点
static int collector_set_point_value(const char* device_name, const char* point_name, double value) {
    if (!device_name || !point_name) return -1;
    
    struct ubus_context *ubus_ctx = ubus_connect(NULL);
    if (!ubus_ctx) {
        HLK_LOG_ERR("[Collector] set_point_value: ubus connect failed\n");
        return -1;
    }

    uint32_t id;
    if (ubus_lookup_id(ubus_ctx, "modbus", &id) != 0) {
        HLK_LOG_ERR("[Collector] set_point_value: Failed to look up modbus object\n");
        ubus_free(ubus_ctx);
        return -1;
    }

    struct blob_buf b = {0};
    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "point", point_name);
    blobmsg_add_double(&b, "value", value);
    blobmsg_add_string(&b, "device", device_name);

    HLK_LOG_INFO("[Collector] set_point_value: invoking set_point_value for device:%s, point:%s, value:%.6f\n", device_name, point_name, value);

    // 调用 ubus，超时时间5000ms
    int ret = ubus_invoke(ubus_ctx, id, "set_point_value", b.head, ubus_invoke_callback, NULL, 5000);
    if (ret != UBUS_STATUS_OK) {
        HLK_LOG_ERR("[Collector] set_point_value: ubus_invoke failed, ret: %d\n", ret);
    } else {
        HLK_LOG_INFO("[Collector] set_point_value: ubus_invoke success\n");
    }

    blob_buf_free(&b);
    ubus_free(ubus_ctx);
    return (ret == UBUS_STATUS_OK) ? 0 : -1;
}

// 专门处理被下发操作的点位的立即上报
static int collector_report_immediate_data(collector_ctx_t *ctx, const char *trace_id, char **point_names, int point_count) {
    if (!ctx || !ctx->targets || ctx->target_count <= 0 || point_count <= 0) return -1;

    // 先同步共享内存获取最新数据
    collector_sync_data(ctx);

    // 估算字符串长度
    size_t estimated_len = 25; // time=xxxxxxxxx&
    for (int i = 0; i < point_count; i++) {
        estimated_len += 50 + 25 + 5; // point_name=数值&
    }

    char *result = (char*)malloc(estimated_len);
    if (!result) return -1;
    result[0] = '\0';
    size_t current_len = 0;

    time_t now = zig_get_timestamp();
    char time_str[25];
    snprintf(time_str, sizeof(time_str), "time=%lld&", now);
    strncat(result, time_str, estimated_len - current_len - 1);
    current_len = strlen(result);

    for (int i = 0; i < point_count; i++) {
        const char *target_point_name = point_names[i];
        
        // 在目标的列表中找当前请求的点位，获取它的最新值
        for (int j = 0; j < ctx->target_count; j++) {
            target_point_t *target = &ctx->targets[j];
            if (strcmp(target->point_name, target_point_name) == 0) {
                char point_str[85];
                if (target->value_type == FLOAT_ABCD || target->value_type == FLOAT_CDAB || target->value_type == FLOAT_DCBA) {
                    snprintf(point_str, sizeof(point_str), "%s=%.*f&", target->point_name, (int)target->decimal_places, target->current_value);
                } else {
                    snprintf(point_str, sizeof(point_str), "%s=%.0f&", target->point_name, target->current_value);
                }
                strncat(result, point_str, estimated_len - current_len - 1);
                current_len = strlen(result);
                break;
            }
        }
    }

    if (current_len > 0 && result[current_len - 1] == '&') {
        result[current_len - 1] = '\0';
        current_len--;
    }

    // 组装完整的 JSON
    cJSON *json_root = cJSON_CreateObject();
    if (!json_root) {
        free(result);
        return -1;
    }

    if (trace_id && strlen(trace_id) > 0) {
        cJSON_AddStringToObject(json_root, "TraceId", trace_id);
    }
    cJSON_AddStringToObject(json_root, "DeviceCode", mqtt_user_cert.deviceName);

    cJSON *items_array = cJSON_CreateArray();
    if (items_array) {
        cJSON *item = cJSON_CreateObject();
        if (item) {
            cJSON_AddNumberToObject(item, "Time", now);
            cJSON_AddStringToObject(item, "Name", "DataPointsUp");
            cJSON_AddStringToObject(item, "Value", result);
            cJSON_AddItemToArray(items_array, item);
        }
        cJSON_AddItemToObject(json_root, "Items", items_array);
    }

    char *response_str = cJSON_PrintUnformatted(json_root);
    cJSON_Delete(json_root);
    free(result);

    int rc = -1;
    if (response_str) {
        if (sharedData.connect_status == MQTT_CONNECT_STATUS_CONNECTED) {
            rc = hlk_mqtt_publish(mqtt_topic_type_table[TOPIC_POST].topic, QOS0, response_str, strlen(response_str));
            if (rc == 0) {
                HLK_LOG_INFO("[Collector] Immediate report sent successfully. TraceId: %s\n", trace_id ? trace_id : "");
            } else {
                HLK_LOG_ERR("[Collector] Immediate report publish failed, rc=%d\n", rc);
            }
        }
        free(response_str);
    }
    return (rc == 0) ? 0 : -1;
}

//处理下发的数据采集
/******************************************************************************
 * 函数名    : hlk_mqtt_handle_data_points_down
 ******************************************************************************/
void hlk_mqtt_handle_data_points_down(cJSON *root)
{
    if (!root) return;
    char *json_str = cJSON_PrintUnformatted(root);
    HLK_LOG_INFO("[Collector] Handle points down: %s\n", json_str);
    free(json_str);

    cJSON *trace_id_obj = cJSON_GetObjectItem(root, "TraceId");
    const char *trace_id = (trace_id_obj && cJSON_IsString(trace_id_obj)) ? trace_id_obj->valuestring : "";

    cJSON *value_obj = cJSON_GetObjectItem(root, "Value");
    if (!value_obj || !cJSON_IsString(value_obj)) {
        HLK_LOG_ERR("[Collector] Handle points down: Value field not found or not string\n");
        return;
    }
    
    char *value_str = strdup(value_obj->valuestring);
    if (!value_str) return;

    if (!g_collector_ctx) {
        HLK_LOG_ERR("[Collector] Handle points down: g_collector_ctx is NULL\n");
        free(value_str);
        return;
    }

    // 分割 value_str 形式如 node0101=1.234&node0202=23.2 或 Device1_state=
    char *saveptr;
    char *token = strtok_r(value_str, "&", &saveptr);
    
    #define MAX_REQ_POINTS 100
    char *requested_points[MAX_REQ_POINTS];
    int req_point_count = 0;

    int did_any_set = 0;

    while (token != NULL && req_point_count < MAX_REQ_POINTS) {
        char *eq_ptr = strchr(token, '=');
        if (eq_ptr) {
            *eq_ptr = '\0';
            char *point_name = token;
            char *point_val_str = eq_ptr + 1;

            // 保存被请求的点位名，用于最后上报
            requested_points[req_point_count] = strdup(point_name);
            if (requested_points[req_point_count]) {
                req_point_count++;
            }

            if (strlen(point_val_str) > 0) {
                // 等号后有值，主动设置
                double set_value = atof(point_val_str);
                
                // 查找 device_name
                const char *device_name = NULL;
                for (int i = 0; i < g_collector_ctx->target_count; i++) {
                    if (strcmp(g_collector_ctx->targets[i].point_name, point_name) == 0) {
                        device_name = g_collector_ctx->targets[i].device_name;
                        break;
                    }
                }

                if (device_name) {
                    collector_set_point_value(device_name, point_name, set_value);
                    did_any_set = 1;
                } else {
                    HLK_LOG_ERR("[Collector] Handle points down: cannot find device_name for point: %s\n", point_name);
                }
            } else {
                // 等号后无值，主动采集 (不操作，后续只上报)
            }
        }
        token = strtok_r(NULL, "&", &saveptr);
    }
    
    free(value_str);

    if (did_any_set) {
        usleep(200 * 1000); // 延时200ms让底层写生效
    }

    // 调用普通的采集上报（全量），如果它返回 1（跳过），则调用我们的即时上报（部分）
    int report_ret = collector_report_data(g_collector_ctx);
    if (report_ret == 1) { // 1 表示 skip report
        collector_report_immediate_data(g_collector_ctx, trace_id, requested_points, req_point_count);
    }

    for (int i = 0; i < req_point_count; i++) {
        free(requested_points[i]);
    }
}
