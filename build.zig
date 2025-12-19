const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // 自定义选项：是否通过 -Dopenwrt=true 快速切换目标
    const openwrt_target = b.option(bool, "openwrt", "Build for OpenWrt (aarch64-linux-musl)") orelse false;

    // 创建 test1 模块
    const mod = b.addModule("test1", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    // 1. 确定常规构建的目标 (zig build)
    const actual_target = if (openwrt_target)
        b.resolveTargetQuery(.{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .musl })
    else
        target;

    // 2. 创建主可执行文件 (常规构建)
    // 这是一个灵活的构建配置，保留了调试能力
    const exe = b.addExecutable(.{
        .name = "test1",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = actual_target,
            .optimize = optimize, // 默认跟随命令行参数 -Doptimize
            .imports = &.{
                .{ .name = "test1", .module = mod },
            },
            // 如果是通过 -Dopenwrt=true 构建，我们也顺便做一下优化
            .strip = if (openwrt_target) true else null,
            .single_threaded = if (openwrt_target) true else null,
        }),
    });
    b.installArtifact(exe);

    // ============================================================
    // 3. OpenWrt 专用构建步骤 (zig build openwrt)
    // 这里我们强制应用所有瘦身策略，确保生成的二进制文件最小
    // ============================================================
    const openwrt_step = b.step("openwrt", "Build for OpenWrt (Production Ready: Small & Stripped)");
    const openwrt_target_query = b.resolveTargetQuery(.{ .cpu_arch = .aarch64, .os_tag = .linux, .abi = .musl });

    const openwrt_exe = b.addExecutable(.{
        .name = "test1",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = openwrt_target_query,

            // 【核心优化点】
            .optimize = .ReleaseSmall, // 强制使用“最小体积”优化，无视命令行参数
            .strip = true, // 强制去除所有符号表 (减小 30%+)
            .single_threaded = true, // 强制单线程 (减少运行时开销)

            .imports = &.{
                .{ .name = "test1", .module = mod },
            },
        }),
    });

    // 将产物安装到 zig-out/openwrt/ 目录下
    const openwrt_install = b.addInstallArtifact(openwrt_exe, .{
        .dest_dir = .{ .override = .{ .custom = "openwrt" } },
    });
    openwrt_step.dependOn(&openwrt_install.step);

    // ============================================================
    // 运行和测试步骤 (保持不变)
    // ============================================================

    // Run step
    const run_step = b.step("run", "Run the app");
    const run_cmd = b.addRunArtifact(exe);
    run_step.dependOn(&run_cmd.step);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    // Tests
    const mod_tests = b.addTest(.{
        .root_module = mod,
    });
    const run_mod_tests = b.addRunArtifact(mod_tests);

    const exe_tests = b.addTest(.{
        .root_module = exe.root_module,
    });
    const run_exe_tests = b.addRunArtifact(exe_tests);

    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_mod_tests.step);
    test_step.dependOn(&run_exe_tests.step);
}
