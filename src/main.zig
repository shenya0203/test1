// 导入Zig标准库
const std = @import("std");
const modbus_collector = @cImport({
    // 导入自定义头文件
    // 路径相对于 src/hlk_cloud/src
    @cInclude("modbus_collector/data_collector.h");
});

// 导入 C 函数声明
// extern 表示这是外部函数声明，通常用于 C 函数
// "c" 表示 C 调用约定
extern "c" fn hi_link_init() c_int;
extern "c" fn sleep(seconds: c_uint) c_int;
// 直接声明C库的休眠函数
extern "c" fn usleep(microseconds: c_uint) c_int;

// 设备五元组(License)操作 C 接口声明
extern "c" fn cfmGetLicense(DN_: [*c]u8, PjK_: [*c]u8, PdK_: [*c]u8, PdS_: [*c]u8, DS_: [*c]u8, size_: usize) c_int;
extern "c" fn cfmSetLicense(DN_: [*c]const u8, PjK_: [*c]const u8, PdK_: [*c]const u8, PdS_: [*c]const u8, DS_: [*c]const u8) c_int;
extern "c" fn cfmClearLicense() c_int;

// 毫秒级休眠函数 - Zig实现，避免C ABI兼容性问题
export fn zig_msleep(msec: c_uint) void {
    // 直接使用C库的usleep函数，参数是微秒
    const usecs = msec * 1000;
    _ = usleep(usecs);
}

// 直接声明C库的时间函数
extern "c" fn time(timer: ?*std.c.time_t) std.c.time_t;

// 定义timeval结构体，与C兼容
pub const ZigTimeval = extern struct {
    tv_sec: std.c.time_t,
    tv_usec: std.c.suseconds_t,
};

// 定义fd_set结构体（简化版本）
pub const ZigFdSet = extern struct {
    fds_bits: [16]u32, // 通常fd_set有1024位，这里简化
};

// 直接声明C库函数
extern "c" fn gettimeofday(tv: ?*std.c.timeval, tz: ?*std.c.timezone) c_int;
extern "c" fn setenv(name: [*:0]const u8, value: [*:0]const u8, overwrite: c_int) c_int;
extern "c" fn tzset() void;
// 定义tm结构体，与C兼容
pub const tm = extern struct {
    tm_sec: c_int, // 秒 (0-59)
    tm_min: c_int, // 分 (0-59)
    tm_hour: c_int, // 时 (0-23)
    tm_mday: c_int, // 日 (1-31)
    tm_mon: c_int, // 月 (0-11)
    tm_year: c_int, // 年 (从1900年开始)
    tm_wday: c_int, // 星期几 (0-6, 0=星期日)
    tm_yday: c_int, // 年中的第几天 (0-365)
    tm_isdst: c_int, // 夏令时标志
    tm_gmtoff: c_long, // 时区偏移（秒）
    tm_zone: [*:0]const u8, // 时区名称
};

extern "c" fn localtime(timer: *const std.c.time_t) ?*tm;

// gettimeofday的Zig实现
export fn zig_gettimeofday(tv: *ZigTimeval, tz: ?*anyopaque) c_int {
    // 使用C库的gettimeofday实现
    const result = gettimeofday(@as(?*std.c.timeval, @ptrCast(@alignCast(tv))), @as(?*std.c.timezone, @ptrCast(@alignCast(tz))));
    return result;
}

// 定义fd_set类型
const fd_set = extern struct {
    fds_bits: [16]u32,
};

// 直接声明C库函数
extern "c" fn select(nfds: c_int, readfds: ?*fd_set, writefds: ?*fd_set, exceptfds: ?*fd_set, timeout: ?*std.c.timeval) c_int;

// select的Zig实现
export fn zig_select(nfds: c_int, readfds: ?*ZigFdSet, writefds: ?*ZigFdSet, exceptfds: ?*ZigFdSet, timeout: ?*ZigTimeval) c_int {
    // 使用C库的select实现
    const result = select(nfds, @as(?*fd_set, @ptrCast(readfds)), @as(?*fd_set, @ptrCast(writefds)), @as(?*fd_set, @ptrCast(exceptfds)), @as(?*std.c.timeval, @ptrCast(@alignCast(timeout))));
    return result;
}

// fd_set操作函数
export fn zig_FD_ZERO(set: *ZigFdSet) void {
    @memset(&set.fds_bits, 0);
}

export fn zig_FD_SET(fd: c_int, set: *ZigFdSet) void {
    if (fd >= 0 and fd < 512) { // 假设最大512个文件描述符
        const idx = @as(usize, @intCast(fd)) / 32;
        const bit = @as(u5, @intCast(@as(u32, @intCast(fd)) % 32)); // 修复：确保bit是u5类型
        set.fds_bits[idx] |= (@as(u32, 1) << bit);
    }
}

export fn zig_FD_ISSET(fd: c_int, set: *ZigFdSet) c_int {
    if (fd >= 0 and fd < 512) {
        const idx = @as(usize, @intCast(fd)) / 32;
        const bit = @as(u5, @intCast(@as(u32, @intCast(fd)) % 32)); // 修复：确保bit是u5类型
        return if ((set.fds_bits[idx] & (@as(u32, 1) << bit)) != 0) 1 else 0;
    }
    return 0;
}

// 获取当前时间戳 - Zig实现，避免C ABI兼容性问题
export fn zig_get_timestamp() i64 {
    // 使用C库的time函数获取当前时间戳
    const now = time(null);
    return @intCast(now);
}

export fn zig_get_month() i64 {
    const t = time(null);
    if (t == -1) return -1;
    const now = localtime(&t) orelse return -1;
    return @intCast(now.tm_mon + 1);
}
export fn zig_get_year() i64 {
    const t = time(null);
    if (t == -1) return -1;
    const now = localtime(&t) orelse return -1;
    return @intCast(now.tm_year + 1900);
}

// 设置时区环境变量 - Zig实现，避免C ABI兼容性问题
export fn zig_set_timezone(tz: [*:0]const u8) c_int {
    // 使用C库的setenv函数设置TZ环境变量
    const result = setenv("TZ", tz, 1);
    if (result == 0) {
        // 设置成功后调用tzset更新时区信息
        tzset();
    }
    return result;
}

// 使用system()设置时区 - 直接调用，避免setenv/tzset问题
extern "c" fn system(command: [*:0]const u8) c_int;

export fn zig_set_timezone_system(_: [*:0]const u8) c_int {
    // 直接使用system()调用设置时区环境变量
    // 格式: TZ=CST-8; export TZ
    _ = system("TZ=CST-8; export TZ");
    return 0; // system()总是返回0表示成功
}

// 计算两个时间戳的绝对差值 - Zig实现，避免fabs阻塞
export fn zig_time_diff_abs(time1: i64, time2: i64) i64 {
    if (time1 > time2) {
        return time1 - time2;
    } else {
        return time2 - time1;
    }
}

// 设置系统时间同步 - Zig实现，使用system()调用避免settimeofday阻塞
export fn zig_set_timesync(timestamp: i64) c_int {
    // 将毫秒时间戳转换为秒
    const seconds = @divTrunc(timestamp, 1000);

    // 构建date命令来设置系统时间
    // 格式: date -s "@timestamp_in_seconds"
    var cmd_buf: [64]u8 = undefined;
    const cmd = std.fmt.bufPrint(&cmd_buf, "date -s \"@{d}\"", .{seconds}) catch {
        return -1; // 格式化失败
    };

    // 使用system()调用执行date命令
    const result = system(@as([*:0]const u8, @ptrCast(cmd.ptr)));
    return result;
}

// 全局静态tm结构体，用于存储localtime结果
// 注意：这不是线程安全的，如果需要线程安全，应该使用线程局部存储
var global_tm: tm = undefined;

// 获取本地时间结构体 - Zig实现
export fn zig_get_localtime(timer: *const std.c.time_t) ?*tm {
    // 直接调用C库的localtime函数
    return localtime(timer);
}

// -----------------------------------------------------------
// Modbus Shared Memory Handler (Zig Implementation) - Final Fix
// -----------------------------------------------------------

const builtin = @import("builtin");

// 定义导出给 C 使用的结构体
pub const ZigShmResult = extern struct {
    fd: c_int,
    ptr: ?*anyopaque,
    size: usize,
    success: c_char,
};

// MT7688 (MIPS32) 的页面大小固定为 4096
// 使用 comptime 常量以满足 @alignCast 的要求
const PAGE_SIZE: usize = 4096;

// 导出函数：打开并映射共享内存
export fn zig_map_modbus_shm(path_c: [*:0]const u8, out_result: *ZigShmResult) void {
    const path = std.mem.span(path_c);
    std.debug.print("[Zig-Shm] Start mapping: {s}\n", .{path});

    out_result.success = 0;
    out_result.fd = -1;
    out_result.ptr = null;
    out_result.size = 0;

    const file = std.fs.openFileAbsolute(path, .{ .mode = .read_only }) catch |err| {
        std.debug.print("[Zig-Shm] Open failed: {}\n", .{err});
        return;
    };
    errdefer file.close();

    const stat = file.stat() catch |err| {
        std.debug.print("[Zig-Shm] Stat failed: {}\n", .{err});
        return;
    };

    std.debug.print("[Zig-Shm] File size: {d} bytes\n", .{stat.size});

    if (stat.size == 0) {
        std.debug.print("[Zig-Shm] Error: File size is 0\n", .{});
        return;
    }

    const size_usize: usize = @intCast(stat.size);

    // 修复1: 使用 @bitCast 强制构造 MIPS 架构的 MAP 标志
    // 在 MIPS Linux 上，std.posix.MAP 是 packed struct(u32)，且 SHARED 对应 0x01
    const map_flags: std.posix.MAP = @bitCast(@as(u32, 1));

    const mmap_ptr = std.posix.mmap(null, size_usize, std.posix.PROT.READ, map_flags, file.handle, 0) catch |err| {
        std.debug.print("[Zig-Shm] Mmap failed: {}\n", .{err});
        return;
    };

    std.debug.print("[Zig-Shm] Mmap success at: {*}\n", .{mmap_ptr.ptr});

    out_result.fd = file.handle;
    out_result.ptr = mmap_ptr.ptr;
    out_result.size = size_usize;
    out_result.success = 1;
}

// 导出函数：清理共享内存
export fn zig_unmap_modbus_shm(ptr: *anyopaque, size: usize, fd: c_int) void {
    std.debug.print("[Zig-Shm] Unmapping ptr={*}, size={d}, fd={d}\n", .{ ptr, size, fd });

    // 修复2: 使用硬编码的 PAGE_SIZE 进行对齐转换
    // 1. 转为非对齐指针
    const u8_ptr = @as([*]u8, @ptrCast(ptr));
    // 2. 转为对齐指针 (PAGE_SIZE 是 comptime known)
    const aligned_ptr: [*]align(PAGE_SIZE) u8 = @alignCast(u8_ptr);
    // 3. 创建切片
    const slice = aligned_ptr[0..size];

    std.posix.munmap(slice);

    if (fd != -1) {
        std.posix.close(fd);
    }
}

// 导出函数：检查共享内存文件是否仍然有效（Inode 检查）
// 返回 true 表示有效，false 表示文件已变更或不存在
export fn zig_check_shm_inode(fd: c_int, path_c: [*:0]const u8) bool {
    const path = std.mem.span(path_c);

    // 1. 获取当前文件路径的 Stat (新状态)
    const file = std.fs.openFileAbsolute(path, .{ .mode = .read_only }) catch {
        // 如果无法打开文件（例如文件被删除了），说明失效
        return false;
    };
    defer file.close();

    const new_stat = file.stat() catch return false;

    // 2. 获取当前持有 FD 的 Stat (旧状态)
    // 在 Zig 中，我们需要通过 std.os (或 std.posix) 调用 fstat
    // 由于 Zig 版本差异，这里使用针对 MT7688 (MIPS) 的底层 stat 结构比较
    // 简单起见，我们假设如果 inode 不匹配，文件就是换了

    // 这里我们直接比较 inode。
    // 注意：std.fs.File.stat() 返回的是抽象的 Stat 结构

    // 获取旧 FD 的 stat
    // 这是一个 trick：为了在 Zig 中对原始 FD 做 stat，我们把它包装成 File
    // 注意：不要 close 这个 dummy_file，否则会关闭外部传入的 fd
    const dummy_file = std.fs.File{ .handle = fd };
    const old_stat = dummy_file.stat() catch return false;

    // 3. 核心判断：比较 Inode
    if (new_stat.inode != old_stat.inode) {
        // Inode 不同，说明文件被删除并重新创建了（生产者重启了）
        return false;
    }

    return true;
}

fn printUsage() void {
    std.debug.print(
        \\Usage: hlk_cloud_zig [options]
        \\
        \\Options:
        \\  -h, --help                 Show this help message and exit
        \\  -g, --get-license          Get and print the device license (5-tuple)
        \\  -c, --clear-license        Clear the device license
        \\  -s, --set-license <DN> <PjK> <PdK> <PdS> <DS>
        \\                             Set the device license with the provided 5-tuple
        \\
        \\If no options are provided, the program will start as a daemon.
        \\
    , .{});
}

// 定义主函数 main()
pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    var args = try std.process.argsWithAllocator(allocator);
    defer args.deinit();

    // 跳过程序名自身
    _ = args.skip();

    if (args.next()) |arg| {
        if (std.mem.eql(u8, arg, "-h") or std.mem.eql(u8, arg, "--help")) {
            printUsage();
            return;
        } else if (std.mem.eql(u8, arg, "-g") or std.mem.eql(u8, arg, "--get-license")) {
            var dn: [64]u8 = undefined;
            var pjk: [64]u8 = undefined;
            var pdk: [64]u8 = undefined;
            var pds: [64]u8 = undefined;
            var ds: [64]u8 = undefined;

            // 初始化为0，确保字符串安全
            @memset(&dn, 0);
            @memset(&pjk, 0);
            @memset(&pdk, 0);
            @memset(&pds, 0);
            @memset(&ds, 0);

            const ret = cfmGetLicense(&dn, &pjk, &pdk, &pds, &ds, 64);
            if (ret == 0) {
                std.debug.print("Get License Success:\n", .{});
                std.debug.print("  DeviceName:    {s}\n", .{std.mem.sliceTo(&dn, 0)});
                std.debug.print("  ProjectKey:    {s}\n", .{std.mem.sliceTo(&pjk, 0)});
                std.debug.print("  ProductKey:    {s}\n", .{std.mem.sliceTo(&pdk, 0)});
                std.debug.print("  ProductSecret: {s}\n", .{std.mem.sliceTo(&pds, 0)});
                std.debug.print("  DeviceSecret:  {s}\n", .{std.mem.sliceTo(&ds, 0)});
            } else {
                std.debug.print("Get License Failed, ret: {d}\n", .{ret});
            }
            return;
        } else if (std.mem.eql(u8, arg, "-c") or std.mem.eql(u8, arg, "--clear-license")) {
            const ret = cfmClearLicense();
            if (ret == 0) {
                std.debug.print("Clear License Success\n", .{});
            } else {
                std.debug.print("Clear License Failed, ret: {d}\n", .{ret});
            }
            return;
        } else if (std.mem.eql(u8, arg, "-s") or std.mem.eql(u8, arg, "--set-license")) {
            var params: [5][]const u8 = undefined;
            var i: usize = 0;
            while (args.next()) |p| {
                if (i < 5) {
                    params[i] = p;
                }
                i += 1;
            }

            if (i != 5) {
                std.debug.print("Error: --set-license requires exactly 5 arguments (DN PjK PdK PdS DS), got {d}\n\n", .{i});
                printUsage();
                return;
            }

            const dn_z = try allocator.dupeZ(u8, params[0]);
            defer allocator.free(dn_z);
            const pjk_z = try allocator.dupeZ(u8, params[1]);
            defer allocator.free(pjk_z);
            const pdk_z = try allocator.dupeZ(u8, params[2]);
            defer allocator.free(pdk_z);
            const pds_z = try allocator.dupeZ(u8, params[3]);
            defer allocator.free(pds_z);
            const ds_z = try allocator.dupeZ(u8, params[4]);
            defer allocator.free(ds_z);

            const ret = cfmSetLicense(dn_z.ptr, pjk_z.ptr, pdk_z.ptr, pds_z.ptr, ds_z.ptr);
            if (ret == 0) {
                std.debug.print("Set License Success\n", .{});
            } else {
                std.debug.print("Set License Failed, ret: {d}\n", .{ret});
            }
            return;
        } else {
            std.debug.print("Unknown argument: {s}\n\n", .{arg});
            printUsage();
            return;
        }
    }

    const collector_ctx: *modbus_collector.collector_ctx_t = modbus_collector.collector_init();
    _ = modbus_collector.collector_register_signal_report_handler();

    // 调用 C 函数启动MQTT主程序

    const result = hi_link_init();

    // // 检查初始化结果
    if (result != 0) {
        std.process.exit(@intCast(result));
    }

    //数据采集初始化
    // 初始化成功后，主线程需要卡住等待，避免进程退出
    // 这样后台线程或服务可以继续运行
    while (true) {
        //std.debug.print("main loop\r\n", .{});
        //modbus_collector.collector_sync_data(collector_ctx);
        if (modbus_collector.collector_take_signal_report_pending() != 0) {
            _ = modbus_collector.collector_report_signal_data(collector_ctx);
        }
        _ = modbus_collector.collector_report_data(collector_ctx);
        // 每秒检查一次，保持进程运行
        _ = sleep(1);
    }
}

// 定义一个测试用例，名称为 "simple test"
// test: 测试块关键字，用于编写单元测试
// Zig的测试系统是内置的，可以通过 zig test 命令运行
test "simple test" {
    // 获取测试分配器，用于测试中的内存管理
    // std.testing.allocator: Zig提供的测试用内存分配器
    // 它会检测内存泄漏，帮助发现内存管理问题
    const gpa = std.testing.allocator;

    // 声明一个动态数组(ArrayList)，元素类型为i32(32位整数)
    // std.ArrayList(i32): 标准库中的动态数组类型
    // var: 变量声明关键字，表示可变变量
    // .empty: 使用空初始化语法，创建空的ArrayList
    var list: std.ArrayList(i32) = .empty;

    // defer: 延迟执行关键字，确保在函数退出时执行清理代码
    // list.deinit(gpa): 释放ArrayList占用的内存
    // 这是Zig的RAII(资源获取即初始化)风格资源管理
    defer list.deinit(gpa); // 尝试注释掉这行，看看Zig是否能检测到内存泄漏！

    // 向ArrayList中添加元素42
    // try: 错误处理，append可能因为内存不足失败
    // gpa: 使用的内存分配器
    // 42: 要添加的整数值
    try list.append(gpa, 42);

    // 验证ArrayList的最后一个元素是否为42
    // std.testing.expectEqual(): 测试断言函数，检查两个值是否相等
    // @as(i32, 42): 类型转换，将字面量42明确转换为i32类型
    // list.pop(): 移除并返回ArrayList的最后一个元素
    try std.testing.expectEqual(@as(i32, 42), list.pop());
}

// 定义一个模糊测试用例，名称为 "fuzz example"
// 模糊测试是一种自动化测试技术，通过随机输入来发现程序错误
test "fuzz example" {
    // 定义一个匿名结构体Context，包含测试逻辑
    // struct { ... }: 匿名结构体定义
    const Context = struct {
        // 定义Context结构体的testOne方法
        // context: @This(): 第一个参数是结构体实例本身，@This()表示当前类型
        // input: []const u8: 参数类型，只读字节数组切片
        // anyerror!void: 返回类型，可能返回任意错误或void
        fn testOne(context: @This(), input: []const u8) anyerror!void {
            // _ = context: 显式忽略未使用的参数，避免编译器警告
            _ = context;

            // 尝试传递 `--fuzz` 参数给 `zig build test`，看看是否能发现这个测试用例的失败！
            // std.testing.expect(): 测试断言，期望条件为true
            // std.mem.eql(): 比较两个字节数组是否相等
            // !: 逻辑非操作符
            // 这个测试期望输入不等于 "canyoufindme"
            try std.testing.expect(!std.mem.eql(u8, "canyoufindme", input));
        }
    };

    // 执行模糊测试
    // Context{}: 创建Context结构体的实例
    // Context.testOne: 传递要测试的函数
    // .{}: 传递给模糊测试器的额外配置选项(空)
    try std.testing.fuzz(Context{}, Context.testOne, .{});
}
