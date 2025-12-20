#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <getopt.h>

#ifdef __MUSL__
// 在musl环境下避免包含time.h，但允许zig的musl提供time_t
// time_t已由zig的musl头文件定义
#else
#include <time.h>
#endif

#include <modbus/modbus.h>
#include <uci.h>
#include "shm_protocol.h"
#include "modbus_collector.h"

#define DEFAULT_CSV_PATH "/etc/config/device/points.csv"

// 数据结构定义
typedef struct {
    char device_path[32]; // e.g., /dev/ttyS0
    int baud;
    char parity;          // 'N', 'E', 'O'
    int data_bit;
    int stop_bit;
} SerialParams;

// 扩展的点位结构，包含共享内存索引
typedef struct Point {
    char name[64];        // 点位名称
    char address_str[16]; // Modbus地址字符串，如"40001"
    int type;             // 4=Uint16, 5=Int16, 18=Bool
    uint16_t address;     // Modbus地址偏移量
    int function_code;    // Modbus功能码
    int is_virtual_status; // 1=虚拟状态点位，0=真实Modbus点位
    uint16_t shm_index;   // 共享内存中的点位索引
    union {
        uint16_t u16_val;
        int16_t i16_val;
        uint8_t bool_val;
    } value;
    struct Point *next;
} Point_t;

typedef struct Device {
    char name[64];        // 设备名
    int protocol;         // 1=RTU, 2=TCP
    int slave_id;
    // TCP 独有
    char ip[32];
    int port;
    // RTU 独有
    char uart_config_name[32]; // CSV SC行 Index 9 (e.g., Uart1)
    SerialParams serial;       // 从 /etc/config/uart 解析出的参数

    struct Point *points;      // 点位链表
    struct Device *next;
} Device_t;

// 全局变量
static volatile int running = 1;
static Device_t *device_list = NULL;
static ShmHeader *shm = NULL;      // 共享内存指针
static size_t shm_size = 65536;        // 共享内存大小
static uint16_t next_shm_index = 0; // 下一个可用的共享内存索引

// 函数声明
void signal_handler(int signum);
int load_devices_from_csv(const char *csv_path);
int load_uart_config(const char *uart_name, SerialParams *out_cfg);
int parse_modbus_address(const char *addr_str, uint16_t *address, int *function_code, int *is_virtual_status);
void free_device_list(Device_t *head);
int init_shm_master(void);
int register_points_to_shm(Device_t *device_list);
int push_point_value(uint16_t shm_index, int type, uint16_t u16_val, int16_t i16_val, uint8_t bool_val);
void *modbus_collection_thread(void *arg);

// 信号处理
void signal_handler(int signum) {
    printf("\nReceived signal %d, shutting down...\n", signum);
    running = 0;
}

// CSV解析函数
int load_devices_from_csv(const char *csv_path) {
    FILE *fp = fopen(csv_path, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open CSV file: %s\n", csv_path);
        return -1;
    }

    printf("Successfully opened CSV file: %s\n", csv_path);

    char line[1024];
    Device_t *current_device = NULL;
    Device_t *device_head = NULL;
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        // 去除换行符
        line[strcspn(line, "\r\n")] = 0;

        printf("Processing line %d: '%s'\n", line_num, line);

        // 跳过空行和注释
        if (strlen(line) == 0 || line[0] == '#') {
            printf("Skipping empty/comment line\n");
            continue;
        }

        // 保存原始行用于手动解析
        char original_line[1024];
        strncpy(original_line, line, sizeof(original_line) - 1);

        char *token = strtok(line, ",");
        if (!token) {
            printf("No token found in line\n");
            continue;
        }

        printf("First token: '%s'\n", token);

        if (strcmp(token, "V") == 0) {
            // 版本行，格式：V,V1.0,N7X0,;
            printf("Found version line, skipping...\n");
            continue;

        } else if (strcmp(token, "SC") == 0) {
            printf("Found SC line, parsing device configuration\n");
            // 设备行: SC,Device1,Desc,1,1,100,0,0,,Uart1,;
            Device_t *device = (Device_t *)malloc(sizeof(Device_t));
            if (!device) {
                fprintf(stderr, "Memory allocation failed for device\n");
                continue;
            }
            memset(device, 0, sizeof(Device_t));

            // 解析设备信息
            token = strtok(NULL, ","); // Device name
            printf("Device name token: '%s'\n", token ? token : "NULL");
            if (token) strncpy(device->name, token, sizeof(device->name) - 1);

            token = strtok(NULL, ","); // Description (跳过)
            printf("Description token: '%s'\n", token ? token : "NULL");

            token = strtok(NULL, ","); // Protocol
            printf("Protocol token: '%s'\n", token ? token : "NULL");
            if (token) device->protocol = atoi(token);

            token = strtok(NULL, ","); // Slave ID
            printf("Slave ID token: '%s'\n", token ? token : "NULL");
            if (token) device->slave_id = atoi(token);

            // 跳过Timeout和一些字段，找到TCP Address
            token = strtok(NULL, ","); // Timeout (skip)
            printf("Timeout token: '%s'\n", token ? token : "NULL");

            // 跳过2个字段
            printf("Skipping 2 more fields...\n");
            for (int i = 0; i < 2; i++) {
                token = strtok(NULL, ",");
                printf("Skipped field %d: '%s'\n", i+1, token ? token : "NULL");
            }

            token = strtok(NULL, ","); // TCP Address (IP:Port) or UART config
            printf("Address/UART token: '%s'\n", token ? token : "NULL");

            // 根据协议和地址内容判断通信方式
            if (token && strlen(token) > 0) {
                // 检查是否包含冒号（IP:Port格式）
                char *colon = strchr(token, ':');
                if (colon) {
                    // TCP模式：IP:Port格式
                    device->protocol = 2; // 强制设置为TCP
                    *colon = '\0';
                    strncpy(device->ip, token, sizeof(device->ip) - 1);
                    device->port = atoi(colon + 1);
                    printf("TCP config detected - IP: %s, Port: %d\n", device->ip, device->port);
                } else {
                    // RTU模式：UART配置名
                    device->protocol = 1; // 强制设置为RTU
                    strncpy(device->uart_config_name, token, sizeof(device->uart_config_name) - 1);
                    printf("RTU config detected - UART name: %s\n", device->uart_config_name);
                }
            } else {
                // 空字段，默认为RTU但需要UART配置
                if (device->protocol == 2) {
                    printf("Warning: TCP protocol specified but no IP:Port provided\n");
                } else {
                    printf("RTU protocol but no UART config provided\n");
                }
            }

            device->points = NULL;
            device->next = NULL;

            printf("Device created: name='%s', protocol=%d, slave_id=%d\n",
                   device->name, device->protocol, device->slave_id);

            // 添加到链表
            if (!device_head) {
                device_head = device;
                current_device = device;
                printf("Set as first device\n");
            } else {
                current_device->next = device;
                current_device = device;
                printf("Added to device list\n");
            }

        } else if (strcmp(token, "C") == 0 && current_device) {
            printf("Found C line, parsing point configuration for device '%s'\n", current_device->name);

            // 使用更可靠的CSV解析：正确处理空字段
            char *fields[50];
            memset(fields, 0, sizeof(fields));

            // 手动解析剩余的CSV字段，正确处理空字段
            char *line_ptr = original_line;
            int field_count = 0;

            // 跳过已经处理的 "C,"
            line_ptr = strchr(line_ptr, ',');
            if (line_ptr) line_ptr++;

            // 解析剩余字段
            while (line_ptr && *line_ptr && field_count < 49) {
                // 跳过前导空格
                while (*line_ptr == ' ') line_ptr++;

                if (*line_ptr == ',') {
                    // 空字段
                    fields[field_count] = "";
                    field_count++;
                    line_ptr++;
                } else if (*line_ptr == '"' || *line_ptr == '\'') {
                    // 引号字段（暂时不支持，跳过）
                    char quote = *line_ptr;
                    line_ptr++;
                    char *start = line_ptr;
                    while (*line_ptr && *line_ptr != quote) line_ptr++;
                    if (*line_ptr == quote) {
                        *line_ptr = '\0';
                        fields[field_count] = start;
                        field_count++;
                        line_ptr++;
                        if (*line_ptr == ',') line_ptr++;
                    }
                } else {
                    // 普通字段
                    char *start = line_ptr;
                    while (*line_ptr && *line_ptr != ',') line_ptr++;
                    if (*line_ptr == ',') {
                        *line_ptr = '\0';
                        line_ptr++;
                    }
                    fields[field_count] = start;
                    field_count++;
                }
            }

            printf("Total fields in C line: %d\n", field_count);
            //for (int i = 0; i < field_count && i < 20; i++) {
            //    printf("C-Line Field[%d]: '%s'\n", i, fields[i] ? fields[i] : "(NULL)");
            //}

            if (field_count < 13) {
                printf("Not enough fields in C line (%d < 13), skipping\n", field_count);
                continue;
            }

            Point_t *point = (Point_t *)malloc(sizeof(Point_t));
            if (!point) {
                fprintf(stderr, "Memory allocation failed for point\n");
                continue;
            }
            memset(point, 0, sizeof(Point_t));

            // 字段0: Parent Device (skip)
            // 字段1: Node Name
            if (fields[1]) strncpy(point->name, fields[1], sizeof(point->name) - 1);

            // 字段3: Data Type
            if (fields[3]) point->type = atoi(fields[3]);

            // 字段11: Modbus Address (对应原来的字段12)
            printf("Modbus Address field[11]: '%s'\n", fields[11] ? fields[11] : "NULL");
            if (fields[11] && strlen(fields[11]) > 0) {
                strncpy(point->address_str, fields[11], sizeof(point->address_str) - 1);
                printf("Parsing Modbus address: '%s'\n", fields[11]);
                if (parse_modbus_address(fields[11], &point->address, &point->function_code, &point->is_virtual_status) != 0) {
                    fprintf(stderr, "Invalid Modbus address: %s\n", fields[11]);
                    free(point);
                    continue;
                }
                printf("Address parsed successfully: address=%d, function_code=%d, is_virtual=%d\n",
                       point->address, point->function_code, point->is_virtual_status);
            } else {
                printf("No valid address found in field[11]\n");
                free(point);
                continue;
            }

            point->next = NULL;

            printf("Point created: name='%s', type=%d, address='%s'\n",
                   point->name, point->type, point->address_str);

            // 添加到设备的点位链表
            if (!current_device->points) {
                current_device->points = point;
                printf("Set as first point for device\n");
            } else {
                Point_t *p = current_device->points;
                while (p->next) p = p->next;
                p->next = point;
                printf("Added to point list\n");
            }
        }
    }

    fclose(fp);

    // 统计信息
    int device_count = 0;
    int total_points = 0;
    Device_t *dev = device_head;
    while (dev) {
        device_count++;
        int point_count = 0;
        Point_t *pt = dev->points;
        while (pt) {
            point_count++;
            pt = pt->next;
        }
        total_points += point_count;
        printf("Device '%s': %d points\n", dev->name, point_count);
        dev = dev->next;
    }

    printf("CSV parsing completed: %d devices, %d total points\n", device_count, total_points);

    device_list = device_head;
    return 0;
}

// UCI UART配置解析函数
int load_uart_config(const char *uart_name, SerialParams *out_cfg) {
    struct uci_context *ctx = NULL;
    struct uci_package *pkg = NULL;
    struct uci_element *e = NULL;
    struct uci_section *s = NULL;
    struct uci_option *o = NULL;
    int ret = -1;

    // 初始化UCI上下文
    ctx = uci_alloc_context();
    if (!ctx) {
        fprintf(stderr, "Failed to allocate UCI context\n");
        return -1;
    }

    // 加载uart配置文件
    if (uci_load(ctx, "uart", &pkg) != UCI_OK) {
        fprintf(stderr, "Failed to load uart config\n");
        goto cleanup;
    }

    // 查找指定的UART配置段
    uci_foreach_element(&pkg->sections, e) {
        s = uci_to_section(e);
        if (strcmp(s->e.name, uart_name) == 0) {
            // 找到匹配的配置段
            break;
        }
        s = NULL;
    }

    if (!s) {
        fprintf(stderr, "UART config '%s' not found\n", uart_name);
        goto cleanup;
    }

    // 解析各个选项
    memset(out_cfg, 0, sizeof(SerialParams));

    // device path
    o = uci_lookup_option(ctx, s, "device");
    if (o && o->type == UCI_TYPE_STRING) {
        strncpy(out_cfg->device_path, o->v.string, sizeof(out_cfg->device_path) - 1);
    }

    // baud_rate
    o = uci_lookup_option(ctx, s, "baud_rate");
    if (o && o->type == UCI_TYPE_STRING) {
        out_cfg->baud = atoi(o->v.string);
    }

    // data_bit
    o = uci_lookup_option(ctx, s, "data_bit");
    if (o && o->type == UCI_TYPE_STRING) {
        out_cfg->data_bit = atoi(o->v.string);
    }

    // stop_bit
    o = uci_lookup_option(ctx, s, "stop_bit");
    if (o && o->type == UCI_TYPE_STRING) {
        out_cfg->stop_bit = atoi(o->v.string);
    }

    // parity
    o = uci_lookup_option(ctx, s, "parity");
    if (o && o->type == UCI_TYPE_STRING) {
        int parity_val = atoi(o->v.string);
        switch (parity_val) {
            case 0: out_cfg->parity = 'N'; break;
            case 1: out_cfg->parity = 'O'; break;
            case 2: out_cfg->parity = 'E'; break;
            default: out_cfg->parity = 'N'; break;
        }
    }

    // 检查是否所有必要字段都已设置
    if (strlen(out_cfg->device_path) == 0 || out_cfg->baud == 0) {
        fprintf(stderr, "Incomplete UART config for '%s'\n", uart_name);
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (pkg) uci_unload(ctx, pkg);
    if (ctx) uci_free_context(ctx);
    return ret;
}

// Modbus地址转换函数
int parse_modbus_address(const char *addr_str, uint16_t *address, int *function_code, int *is_virtual_status) {
    printf("parse_modbus_address called with: '%s'\n", addr_str ? addr_str : "NULL");

    if (!addr_str) {
        printf("Address string is NULL\n");
        return -1;
    }

    // 检查是否为虚拟状态点位
    if (strcasecmp(addr_str, "State") == 0) {
        printf("Detected virtual status point\n");
        *is_virtual_status = 1;
        *address = 0;
        *function_code = 0;
        printf("Virtual status point parsed successfully\n");
        return 0;
    }

    *is_virtual_status = 0; // 重置为非虚拟状态

    char *endptr;
    long full_addr = strtol(addr_str, &endptr, 10);
    printf("Full address number: %ld\n", full_addr);

    if (*endptr != '\0') {
        printf("Invalid characters in address\n");
        return -1;
    }

    if (full_addr < 1 || full_addr > 499999) {
        printf("Address out of range (1-499999): %ld\n", full_addr);
        return -1;
    }

    // 根据地址范围确定功能码和偏移量
    if (full_addr >= 400001 && full_addr <= 465535) {
        // 4xxxxx: Holding Registers
        *function_code = 0x03;
        *address = full_addr - 400001;
        printf("Function code: 0x03 (Holding Registers), offset: %d\n", *address);
    } else if (full_addr >= 300001 && full_addr <= 365535) {
        // 3xxxxx: Input Registers
        *function_code = 0x04;
        *address = full_addr - 300001;
        printf("Function code: 0x04 (Input Registers), offset: %d\n", *address);
    } else if (full_addr >= 100001 && full_addr <= 165535) {
        // 1xxxxx: Input Status (Discrete Inputs)
        *function_code = 0x02;
        *address = full_addr - 100001;
        printf("Function code: 0x02 (Input Status), offset: %d\n", *address);
    } else if (full_addr >= 1 && full_addr <= 65535) {
        // 0xxxxx or just xxxxx: Coil Status
        *function_code = 0x01;
        *address = full_addr - 1;
        printf("Function code: 0x01 (Coil Status), offset: %d\n", *address);
    } else {
        printf("Invalid address format: %ld\n", full_addr);
        return -1;
    }

    printf("Address parsing successful\n");
    return 0;
}

// 共享内存初始化函数
int init_shm_master(void) {
    if (shm_init_master(&shm, &shm_size) != 0) {
        fprintf(stderr, "Failed to initialize shared memory master\n");
        return -1;
    }
    printf("Successfully initialized shared memory master shm:%p\n", shm);
    return 0;
}

// 注册所有点位到共享内存
int register_points_to_shm(Device_t *device_list) {
    Device_t *device = device_list;
    int total_points = 0;

    while (device) {
        Point_t *point = device->points;
        while (point) {
            if (next_shm_index >= MAX_POINTS) {
                fprintf(stderr, "Too many points, maximum is %d\n", MAX_POINTS);
                return -1;
            }

            // 确定共享内存中的数据类型
            uint8_t shm_type;
            switch (point->type) {
                case 4:  // Uint16
                    shm_type = SHM_TYPE_UINT16;
                    break;
                case 5:  // Int16
                    shm_type = SHM_TYPE_INT16;
                    break;
                case 18: // Bool
                    shm_type = SHM_TYPE_BOOL;
                    break;
                default:
                    fprintf(stderr, "Unsupported point type: %d for %s.%s\n",
                            point->type, device->name, point->name);
                    shm_type = SHM_TYPE_UINT16; // 默认类型
                    break;
            }

            // 注册点位到共享内存
            if (shm_set_point_info(shm, next_shm_index,
                                  device->name, point->name, shm_type) != 0) {
                fprintf(stderr, "Failed to register point %s.%s to SHM\n",
                        device->name, point->name);
                return -1;
            }

            // 保存共享内存索引到点位结构
            point->shm_index = next_shm_index;
            next_shm_index++;
            total_points++;

            point = point->next;
        }
        device = device->next;
    }

    printf("Registered %d points to shared memory\n", total_points);
    return 0;
}

// 推送点位值变化到共享内存
int push_point_value(uint16_t shm_index, int type, uint16_t u16_val, int16_t i16_val, uint8_t bool_val) {
    ShmValue val;

    // 根据数据类型设置值
    switch (type) {
        case 4:  // Uint16
            val.u16 = u16_val;
            break;
        case 5:  // Int16
            val.i16 = i16_val;
            break;
        case 18: // Bool
            val.b8 = bool_val;
            break;
        default:
            return -1; // 不支持的类型
    }

    return shm_push_change(shm, shm_index, val);
}

// Modbus采集线程函数
void *modbus_collection_thread(void *arg) {
    (void)arg; // 标记参数为未使用，避免警告
    printf("Modbus collection thread started\n");

    while (running) {
        Device_t *device = device_list;

        // 遍历所有设备
        while (device && running) {
            modbus_t *ctx = NULL;
            int ret;

            // 添加时间戳和设备信息
#ifdef __MUSL__
            // 在musl环境下使用固定时间戳，完全避免time()调用
            printf("[1000000000 %s] ", device->name);
#else
            time_t now = time(NULL);
            printf("[%ld %s] ", (long)now, device->name);
#endif
            fflush(stdout);

            // 根据协议类型创建Modbus上下文
            if (device->protocol == 1) { // RTU
                if (strlen(device->serial.device_path) == 0) {
                    fprintf(stderr, "RTU device %s has no serial config\n", device->name);
                    device = device->next;
                    continue;
                }

                ctx = modbus_new_rtu(device->serial.device_path,
                                    device->serial.baud,
                                    device->serial.parity,
                                    device->serial.data_bit,
                                    device->serial.stop_bit);
                if (!ctx) {
                    fprintf(stderr, "Failed to create RTU context for %s\n", device->name);
                    device = device->next;
                    continue;
                }

                modbus_set_slave(ctx, device->slave_id);

            } else if (device->protocol == 2) { // TCP
                if (strlen(device->ip) == 0) {
                    fprintf(stderr, "TCP device %s has no IP config\n", device->name);
                    device = device->next;
                    continue;
                }

                ctx = modbus_new_tcp(device->ip, device->port);
                if (!ctx) {
                    fprintf(stderr, "Failed to create TCP context for %s\n", device->name);
                    device = device->next;
                    continue;
                }

                modbus_set_slave(ctx, device->slave_id);

            } else {
                fprintf(stderr, "Unsupported protocol %d for device %s\n",
                       device->protocol, device->name);
                device = device->next;
                continue;
            }

            // 设置响应超时
            uint32_t timeout_sec = 1;  // 1秒超时
            uint32_t timeout_usec = 0;
            modbus_set_response_timeout(ctx, timeout_sec, timeout_usec);

            // 连接到设备
            ret = modbus_connect(ctx);
            if (ret < 0) {
                fprintf(stderr, "Connection failed for device %s: %s\n",
                       device->name, modbus_strerror(errno));
                modbus_free(ctx);
                device = device->next;
                continue;
            }

            // 读取设备的所有点位
            Point_t *point = device->points;
            int has_valid_data = 0;
            int device_online = 1; // 假设设备在线，除非连接或读取失败

            while (point && running) {
                // 跳过虚拟状态点位，这些将在后面根据设备状态设置
                if (point->is_virtual_status) {
                    point = point->next;
                    continue;
                }

                uint16_t reg_value;
                uint8_t coil_value;

                // 根据功能码读取数据
                switch (point->function_code) {
                    case 0x01: // Read Coils
                        ret = modbus_read_bits(ctx, point->address, 1, &coil_value);
                        if (ret == 1) {
                            point->value.bool_val = coil_value;
                            printf("%s=%d ", point->name, point->value.bool_val);
                            has_valid_data = 1;
                        } else {
                            fprintf(stderr, "Failed to read coil %s: %s\n",
                                   point->address_str, modbus_strerror(errno));
                            device_online = 0; // 读取失败认为设备离线
                        }
                        break;

                    case 0x02: // Read Discrete Inputs
                        ret = modbus_read_input_bits(ctx, point->address, 1, &coil_value);
                        if (ret == 1) {
                            point->value.bool_val = coil_value;
                            printf("%s=%d ", point->name, point->value.bool_val);
                            has_valid_data = 1;
                        } else {
                            fprintf(stderr, "Failed to read discrete input %s: %s\n",
                                   point->address_str, modbus_strerror(errno));
                            device_online = 0;
                        }
                        break;

                    case 0x03: // Read Holding Registers
                        ret = modbus_read_registers(ctx, point->address, 1, &reg_value);
                        if (ret == 1) {
                            if (point->type == 4) { // Uint16
                                point->value.u16_val = reg_value;
                                printf("%s=%u ", point->name, point->value.u16_val);
                            } else if (point->type == 5) { // Int16
                                point->value.i16_val = (int16_t)reg_value;
                                printf("%s=%d ", point->name, point->value.i16_val);
                            } else if (point->type == 18) { // Bool
                                point->value.bool_val = reg_value != 0;
                                printf("Point %s (addr: %s) value: %d\n",
                                       point->name, point->address_str, point->value.bool_val);
                            }
                            has_valid_data = 1;
                        } else {
                            fprintf(stderr, "Failed to read holding register %s: %s\n",
                                   point->address_str, modbus_strerror(errno));
                            device_online = 0;
                        }
                        break;

                    case 0x04: // Read Input Registers
                        ret = modbus_read_input_registers(ctx, point->address, 1, &reg_value);
                        if (ret == 1) {
                            if (point->type == 4) { // Uint16
                                point->value.u16_val = reg_value;
                                printf("%s=%u ", point->name, point->value.u16_val);
                            } else if (point->type == 5) { // Int16
                                point->value.i16_val = (int16_t)reg_value;
                                printf("%s=%d ", point->name, point->value.i16_val);
                            } else if (point->type == 18) { // Bool
                                point->value.bool_val = reg_value != 0;
                                printf("Point %s (addr: %s) value: %d\n",
                                       point->name, point->address_str, point->value.bool_val);
                            }
                            has_valid_data = 1;
                        } else {
                            fprintf(stderr, "Failed to read input register %s: %s\n",
                                   point->address_str, modbus_strerror(errno));
                            device_online = 0;
                        }
                        break;

                    default:
                        fprintf(stderr, "Unsupported function code %d for %s\n",
                               point->function_code, point->address_str);
                        device_online = 0;
                        break;
                }

                point = point->next;
            }

            // 如果连接就失败了，认为设备离线
            if (ret < 0) {
                device_online = 0;
            }

            // 设置虚拟状态点位的值
            point = device->points;
            while (point) {
                if (point->is_virtual_status) {
                    point->value.bool_val = device_online;
                    printf("%s=%d ", point->name, point->value.bool_val);
                    has_valid_data = 1; // 虚拟点位也算有效数据
                }
                point = point->next;
            }

            // 如果有有效数据，推送到共享内存
            if (has_valid_data) {
                Point_t *point = device->points;
                while (point) {
                    // 推送每个点位的值变化
                    push_point_value(point->shm_index, point->type,
                                   point->value.u16_val,
                                   point->value.i16_val,
                                   point->value.bool_val);
                    point = point->next;
                }
            }

            // 关闭连接
            modbus_close(ctx);
            modbus_free(ctx);

            // 每个设备采集完成后换行并刷新输出缓冲区
            printf("\n");
            fflush(stdout);

            device = device->next;
        }

        // 采集间隔
        sleep(5);
    }

    printf("Modbus collection thread exiting\n");
    return NULL;
}

// 内存清理函数
void free_device_list(Device_t *head) {
    Device_t *device = head;
    while (device) {
        Device_t *next_device = device->next;

        // 释放点位链表
        Point_t *point = device->points;
        while (point) {
            Point_t *next_point = point->next;
            free(point);
            point = next_point;
        }

        free(device);
        device = next_device;
    }
}

int start_modbus_collector(int argc, char *argv[]) {
    int opt;
    char *csv_path = DEFAULT_CSV_PATH;
    pthread_t collection_tid;

    // 解析命令行参数
    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        switch (opt) {
            case 'c':
                csv_path = optarg;
                break;
            case 'h':
            default:
                printf("Usage: %s [-c csv_path]\n", argv[0]);
                printf("  -c: CSV config file path (default: %s)\n", DEFAULT_CSV_PATH);
                return EXIT_SUCCESS;
        }
    }

    printf("Modbus Collector v1.0.0 (SHM version) starting...\n");
    printf("CSV config: %s\n", csv_path);

    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 加载CSV配置文件
    printf("Loading CSV configuration...\n");
    if (load_devices_from_csv(csv_path) != 0) {
        fprintf(stderr, "Failed to load CSV configuration\n");
        return EXIT_FAILURE;
    }

    if (!device_list) {
        fprintf(stderr, "No devices found in configuration\n");
        shm_cleanup(shm);
        return EXIT_FAILURE;
    }

    // 为RTU设备加载UART配置
    printf("Loading UART configurations...\n");
    Device_t *device = device_list;
    while (device) {
        if (device->protocol == 1) { // RTU
            if (strlen(device->uart_config_name) > 0) {
                if (load_uart_config(device->uart_config_name, &device->serial) != 0) {
                    fprintf(stderr, "Failed to load UART config for device %s, skipping...\n",
                           device->name);
                    // 可以选择标记设备为无效，或者继续处理其他设备
                } else {
                    printf("Loaded UART config for %s: %s@%d baud\n",
                           device->name, device->serial.device_path, device->serial.baud);
                }
            } else {
                fprintf(stderr, "RTU device %s has no UART config name\n", device->name);
            }
        }
        device = device->next;
    }
    // 初始化共享内存
    printf("Initializing shared memory master...\n");
    if (init_shm_master() != 0) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        free_device_list(device_list);
        return EXIT_FAILURE;
    }

    // 注册所有点位到共享内存
    printf("Registering points to shared memory...\n");
    if (register_points_to_shm(device_list) != 0) {
        fprintf(stderr, "Failed to register points to shared memory\n");
        shm_cleanup(shm);
        free_device_list(device_list);
        return EXIT_FAILURE;
    }

    // 创建Modbus采集线程
    printf("Starting Modbus collection thread...\n");
    if (pthread_create(&collection_tid, NULL, modbus_collection_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create collection thread\n");
        shm_cleanup(shm);
        free_device_list(device_list);
        return EXIT_FAILURE;
    }

    printf("Modbus Collector is running. Press Ctrl+C to stop...\n");

    // 主循环
    while (running) {
        sleep(1);
    }

    // 等待采集线程结束
    pthread_join(collection_tid, NULL);

    // 清理资源
    shm_cleanup(shm);
    free_device_list(device_list);

    printf("Modbus Collector stopped.\n");
    return EXIT_SUCCESS;
}
