// 导入Zig标准库
const std = @import("std");

// 导入 C 函数声明
// extern 表示这是外部函数声明，通常用于 C 函数
// "c" 表示 C 调用约定
extern "c" fn hi_link_init() c_int;
extern "c" fn sleep(seconds: c_uint) c_int;

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
