const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // 1. 定义 MT7688 (OpenWrt) 的专用目标参数
    const mt7688_target_query = b.resolveTargetQuery(.{
        .cpu_arch = .mipsel, // MIPS 小端序
        .os_tag = .linux, // 运行 Linux
        .abi = .musl, // OpenWrt 强制使用 musl 以实现静态链接
        .cpu_model = .{ .explicit = &std.Target.mips.cpu.mips32r2 }, // 适配 MT7688 内核
    });

    // 1. 定义 MT7621 的目标查询
    // MT7621 属于 mipsel (小端)，内核为 1004Kc
    const mt7621_target_query = b.resolveTargetQuery(.{
        .cpu_arch = .mipsel,
        .os_tag = .linux,
        .abi = .musl, // LEDE/OpenWrt 统一使用 musl
        // Zig 内部 mips32r2 是最匹配 MT7621 (1004Kc) 的指令集选项
        .cpu_model = .{ .explicit = &std.Target.mips.cpu.mips32r2 },
    });

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
    // 4. MT7688 专用构建命令 (zig build mt7688)
    // 直接执行 zig build mt7688 即可生成最小化的 MIPS 二进制文件
    // ============================================================
    const mt7688_step = b.step("mt7688", "Build for MT7688 OpenWrt (Small & Stripped)");

    const mt7688_exe = b.addExecutable(.{
        .name = "test1_mt7688",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = mt7688_target_query,

            // 针对 MT7688 极小内存环境的优化
            .optimize = .ReleaseSmall,
            .strip = true, // 移除符号表，大幅减小体积
            .single_threaded = true, // MT7688 是单核 CPU，禁用多线程支持可减小体积

            .imports = &.{
                .{ .name = "test1", .module = mod },
            },
        }),
    });

    // 将产物安装到 zig-out/mt7688/ 目录下
    const mt7688_install = b.addInstallArtifact(mt7688_exe, .{
        .dest_dir = .{ .override = .{ .custom = "mt7688" } },
    });
    mt7688_step.dependOn(&mt7688_install.step);

    // ============================================================
    // 2. MT7621 专用构建步骤 (zig build mt7621)
    // ============================================================
    const mt7621_step = b.step("mt7621", "Build for MT7621 (1004Kc, Multi-thread)");

    const mt7621_exe = b.addExecutable(.{
        .name = "test1_mt7621",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = mt7621_target_query,

            // 优化选项
            .optimize = .ReleaseSmall, // 考虑到路由器 Flash 空间通常较小
            .strip = true, // 必须移除符号表以减小体积

            // 注意：MT7621 是多核多线程，这里不再设置 single_threaded = true
            // 以便利用它的多核性能

            .imports = &.{
                .{ .name = "test1", .module = mod },
            },
        }),
    });

    // 安装到 zig-out/mt7621/
    const mt7621_install = b.addInstallArtifact(mt7621_exe, .{
        .dest_dir = .{ .override = .{ .custom = "mt7621" } },
    });
    mt7621_step.dependOn(&mt7621_install.step);

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
