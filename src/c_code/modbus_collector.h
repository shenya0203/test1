#ifndef MODBUS_COLLECTOR_H
#define MODBUS_COLLECTOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Modbus 采集器
 *
 * 这是一个重命名的 main 函数，用于从 Zig 代码调用。
 * 原来的 main 函数被重命名为 start_modbus_collector 以便被 Zig 调用。
 *
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return int 返回值 (0 表示成功)
 */
int start_modbus_collector(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_COLLECTOR_H */
