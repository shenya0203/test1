// 导入Zig标准库
const std = @import("std");

// 导入 C 函数声明
// extern 表示这是外部函数声明，通常用于 C 函数
// "c" 表示 C 调用约定
extern "c" fn hi_link_init() c_int;
extern "c" fn sleep(seconds: c_uint) c_int;

// 直接声明C库的休眠函数
extern "c" fn usleep(microseconds: c_uint) c_int;

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

// 定义主函数 main()
pub fn main() !void {
    // 调用 C 函数启动MQTT主程序
    const result = hi_link_init();

    // 检查初始化结果
    if (result != 0) {
        std.process.exit(@intCast(result));
    }

    // 初始化成功后，主线程需要卡住等待，避免进程退出
    // 这样后台线程或服务可以继续运行
    while (true) {
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
