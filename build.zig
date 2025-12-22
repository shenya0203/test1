const std = @import("std");
const builtin = @import("builtin");

// 平台配置结构体
const PlatformConfig = struct {
    name: []const u8, // 目标名称 (mt7688, mt7981, mt7621)
    product_id: []const u8, // 产品ID (7628, RM65, RM60)
    arch: std.Target.Query, // 目标架构
    macro: []const u8, // C宏定义
    single_threaded: bool, // 是否单线程
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // 定义平台配置列表
    const platforms = [_]PlatformConfig{
        .{
            .name = "mt7688",
            .product_id = "7628",
            .arch = .{
                .cpu_arch = .mipsel,
                .os_tag = .linux,
                .abi = .musleabi,
                .cpu_model = .{ .explicit = &std.Target.mips.cpu.mips32r2 },
            },
            .macro = "HLK_PRODUCT_7628",
            .single_threaded = true, // MT7688 是单核 CPU
        },
        .{
            .name = "mt7981",
            .product_id = "RM65",
            .arch = .{
                .cpu_arch = .aarch64,
                .os_tag = .linux,
                .abi = .musleabihf,
            },
            .macro = "HLK_PRODUCT_RM65",
            .single_threaded = false, // MT7981 支持多线程
        },
        .{
            .name = "mt7621",
            .product_id = "RM60",
            .arch = .{
                .cpu_arch = .mipsel,
                .os_tag = .linux,
                .abi = .musleabi,
                .cpu_model = .{ .explicit = &std.Target.mips.cpu.mips32r2 },
            },
            .macro = "HLK_PRODUCT_RM60",
            .single_threaded = false, // MT7621 支持多线程
        },
    };

    // 创建默认模块 (用于测试等)
    const mod = b.addModule("hlk_cloud", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    // 为每个平台创建构建步骤
    for (platforms) |platform| {
        createPlatformBuildStep(b, platform, mod);
    }

    // 生成通用的版本头文件（在默认构建时生成）
    //const default_version_file = "src/hlk_cloud/src/include/hi_cfm_version.h";
    //if (std.fs.cwd().statFile(default_version_file)) |_| {
    //    std.log.info("Default version header already exists: {s}", .{default_version_file});
    //} else |_| {
    //    generateVersionHeader(b, "HLK", default_version_file);
    //}

    // 默认构建步骤 (用于开发和测试)
    const exe = b.addExecutable(.{
        .name = "hlk_cloud",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "hlk_cloud", .module = mod },
            },
        }),
    });
    b.installArtifact(exe);

    // 运行步骤
    const run_step = b.step("run", "Run the app");
    const run_cmd = b.addRunArtifact(exe);
    run_step.dependOn(&run_cmd.step);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    // 测试步骤
    const exe_tests = b.addTest(.{
        .root_module = exe.root_module,
    });
    const run_exe_tests = b.addRunArtifact(exe_tests);

    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_exe_tests.step);

    // 清理步骤
    const clean_step = b.step("clean", "Clean build artifacts");
    clean_step.dependOn(&b.addRemoveDirTree(.{ .cwd_relative = "zig-out" }).step);
    clean_step.dependOn(&b.addRemoveDirTree(.{ .cwd_relative = ".zig-cache" }).step);
}

// 为特定平台创建构建步骤
fn createPlatformBuildStep(b: *std.Build, platform: PlatformConfig, mod: *std.Build.Module) void {
    const step = b.step(platform.name, std.fmt.allocPrint(b.allocator, "Build for {s} ({s})", .{ platform.name, platform.product_id }) catch unreachable);

    // 为当前平台生成版本头文件（如果不存在）
    const version_file_path = std.fmt.allocPrint(b.allocator, "src/hlk_cloud/src/include/hi_cfm_version_{s}.h", .{platform.name}) catch unreachable;

    // 使用 WriteFile 步骤来生成版本头文件，确保只在需要时生成
    const version_step = b.addWriteFile(version_file_path, "");
    step.dependOn(&version_step.step);

    // 只有当文件不存在时才生成新的版本头文件
    if (std.fs.cwd().statFile(version_file_path)) |_| {
        std.log.info("Version header already exists: {s}", .{version_file_path});
    } else |_| {
        generateVersionHeader(b, platform.product_id, version_file_path);
    }

    // 创建可执行文件
    const exe = b.addExecutable(.{
        .name = std.fmt.allocPrint(b.allocator, "hlk_cloud_{s}", .{platform.name}) catch unreachable,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = b.resolveTargetQuery(platform.arch),
            .optimize = .ReleaseSmall, // 嵌入式设备使用最小体积优化
            .strip = true, // 移除符号表减少体积
            .single_threaded = platform.single_threaded,
            .imports = &.{
                .{ .name = "hlk_cloud", .module = mod },
            },
            .link_libc = true, // 明确链接libc以支持C代码
        }),
    });

    // 对于交叉编译，允许未定义符号（动态库在目标系统上提供）
    exe.linker_allow_shlib_undefined = true;

    // 头文件依赖已在上面添加

    // 添加C源文件
    addCSourceFiles(b, exe, platform.product_id, platform.macro);

    // 添加包含路径和库依赖
    addPlatformDependencies(b, exe, platform.name);

    // 安装到指定目录
    const install = b.addInstallArtifact(exe, .{
        .dest_dir = .{ .override = .{ .custom = platform.name } },
    });
    step.dependOn(&install.step);
}

// 生成版本头文件
fn generateVersionHeader(b: *std.Build, product_id: []const u8, file_path: []const u8) void {
    // 使用系统命令获取当前日期 (Windows兼容)
    const result = std.process.Child.run(.{
        .allocator = b.allocator,
        .argv = &[_][]const u8{ "powershell", "-Command", "Get-Date -Format 'yyMMdd.HHmmss'" },
        .cwd = ".",
    }) catch |err| {
        std.log.err("Failed to get current date: {}", .{err});
        // 回退到固定日期
        const version_content = std.fmt.allocPrint(b.allocator, "#define AT_VERSION \"{s}-1.0.0-{s}\"\n", .{ product_id, "20251222" }) catch unreachable;

        std.fs.cwd().writeFile(.{
            .sub_path = file_path,
            .data = version_content,
        }) catch |write_err| {
            std.log.err("Failed to write version header file {s}: {}", .{ file_path, write_err });
            std.process.exit(1);
        };
        std.log.info("Generated version header: {s}", .{file_path});
        return;
    };
    defer b.allocator.free(result.stdout);
    defer b.allocator.free(result.stderr);

    if (result.term.Exited != 0) {
        std.log.err("Command failed with exit code {}", .{result.term.Exited});
        // 回退到固定日期
        const version_content = std.fmt.allocPrint(b.allocator, "#define AT_VERSION \"{s}-1.0.0-{s}\"\n", .{ product_id, "20251222" }) catch unreachable;

        std.fs.cwd().writeFile(.{
            .sub_path = file_path,
            .data = version_content,
        }) catch |write_err| {
            std.log.err("Failed to write version header file {s}: {}", .{ file_path, write_err });
            std.process.exit(1);
        };
        std.log.info("Generated version header: {s}", .{file_path});
        return;
    }

    // 移除换行符和空格
    const date_str_raw = std.mem.trim(u8, result.stdout, " \t\r\n");
    const date_str = std.fmt.allocPrint(b.allocator, "{s}", .{date_str_raw}) catch unreachable;

    // 生成版本字符串
    const version_content = std.fmt.allocPrint(b.allocator, "#define AT_VERSION \"{s}-1.0.0-{s}\"\n", .{ product_id, date_str }) catch unreachable;

    // 直接写入文件
    std.fs.cwd().writeFile(.{
        .sub_path = file_path,
        .data = version_content,
    }) catch |err| {
        std.log.err("Failed to write version header file {s}: {}", .{ file_path, err });
        std.process.exit(1);
    };
    std.log.info("Generated version header: {s}", .{file_path});
}

// 添加C源文件
fn addCSourceFiles(b: *std.Build, exe: *std.Build.Step.Compile, product_id: []const u8, macro: []const u8) void {
    var arena = std.heap.ArenaAllocator.init(b.allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    // 通用编译标志
    const common_flags = [_][]const u8{
        "-std=gnu99",
        "-DSUPPORT_OPENWRT",
        "-DENABLE_CURL",
        "-D__MUSL__", // 明确标识为Musl环境
        // 强制32位时间ABI兼容性 (与GCC 8.4 musl legacy环境匹配)
        "-U_TIME_BITS", // 取消默认的_TIME_BITS定义
        "-D_TIME_BITS=32", // 强制使用32位时间类型
        "-D_FILE_OFFSET_BITS=32", // 强制使用32位文件偏移
        "-D__USE_TIME_BITS64=0", // 明确禁用64位时间
        "-D_SYSCALL_WORDSIZE=32", // 强制32位系统调用
        std.fmt.allocPrint(allocator, "-D{s}", .{macro}) catch unreachable,
    };

    // 包含路径
    const include_paths = [_][]const u8{
        "src/hlk_cloud/src/include", // 主头文件目录
        "src/hlk_cloud/src/source", // 源文件目录中的头文件
        "src/hlk_cloud/src", // 支持相对包含路径如 MQTTPacket/MQTTPacket.h
        std.fmt.allocPrint(allocator, "src/hlk_cloud/src/platform/{s}", .{product_id}) catch unreachable,
    };

    // 收集所有C源文件
    var c_files = std.ArrayList([]const u8).initCapacity(allocator, 0) catch unreachable;

    // 添加通用目录的C文件
    addCSourcesFromDir(allocator, &c_files, "src/hlk_cloud/src/source");
    addCSourcesFromDir(allocator, &c_files, "src/hlk_cloud/src/MQTTPacket");
    addCSourcesFromDir(allocator, &c_files, "src/hlk_cloud/src/client");
    addCSourcesFromDir(allocator, &c_files, "src/hlk_cloud/src/openwrt");
    addCSourcesFromDir(allocator, &c_files, "src/hlk_cloud/src/linux");

    // 添加特定平台的C文件
    const platform_dir = std.fmt.allocPrint(allocator, "src/hlk_cloud/src/platform/{s}", .{product_id}) catch unreachable;
    addCSourcesFromDir(allocator, &c_files, platform_dir);

    // 构建完整的编译标志列表
    var flags = std.ArrayList([]const u8).initCapacity(allocator, 0) catch unreachable;
    flags.appendSlice(allocator, &common_flags) catch unreachable;

    // 添加包含路径
    for (include_paths) |path| {
        flags.append(allocator, "-I") catch unreachable;
        flags.append(allocator, path) catch unreachable;
    }

    // 为每个C文件添加编译步骤
    for (c_files.items) |c_file| {
        exe.addCSourceFile(.{
            .file = b.path(c_file),
            .flags = flags.items,
        });
    }

    // On 32-bit MIPS musl (OpenWrt with musl 1.2.x) the runtime doesn't export
    // glibc-style __*time64 symbols. The musl compatibility shim has been
    // embedded directly in hi_mqtt.c to ensure it's always linked.
    // No separate compilation unit needed.

    std.log.info("Added {d} C source files for platform {s}", .{ c_files.items.len, product_id });
}

// 递归添加目录中的C文件
fn addCSourcesFromDir(allocator: std.mem.Allocator, files: *std.ArrayList([]const u8), dir_path: []const u8) void {
    var dir = std.fs.cwd().openDir(dir_path, .{ .iterate = true }) catch |err| {
        std.log.warn("Failed to open directory {s}: {}", .{ dir_path, err });
        return;
    };
    defer dir.close();

    var walker = dir.walk(allocator) catch |err| {
        std.log.warn("Failed to walk directory {s}: {}", .{ dir_path, err });
        return;
    };
    defer walker.deinit();

    // 有问题的文件列表，暂时跳过
    const skip_files = [_][]const u8{};

    while (walker.next() catch |err| {
        std.log.warn("Failed to walk entry in {s}: {}", .{ dir_path, err });
        return;
    }) |entry| {
        if (entry.kind == .file and std.mem.endsWith(u8, entry.path, ".c")) {
            // 检查是否是要跳过的文件
            var should_skip = false;
            for (skip_files) |skip_file| {
                if (std.mem.eql(u8, entry.path, skip_file)) {
                    should_skip = true;
                    std.log.warn("Skipping problematic file: {s}", .{entry.path});
                    break;
                }
            }

            if (!should_skip) {
                const full_path = std.fs.path.join(allocator, &[_][]const u8{ dir_path, entry.path }) catch |err| {
                    std.log.warn("Failed to join path {s}/{s}: {}", .{ dir_path, entry.path, err });
                    continue;
                };
                files.append(allocator, full_path) catch |err| {
                    std.log.warn("Failed to append file {s}: {}", .{ full_path, err });
                    continue;
                };
            }
        }
    }
}

// 添加平台特定的依赖
fn addPlatformDependencies(b: *std.Build, exe: *std.Build.Step.Compile, platform_name: []const u8) void {
    const platform_path = std.fmt.allocPrint(b.allocator, "src/platform/{s}", .{platform_name}) catch unreachable;

    // 检查平台目录是否存在
    var platform_dir = std.fs.cwd().openDir(platform_path, .{}) catch |err| {
        if (err == error.FileNotFound) {
            std.log.warn("Platform directory {s} not found, skipping platform-specific libraries", .{platform_path});
            return;
        }
        std.log.err("Failed to open platform directory {s}: {}", .{ platform_path, err });
        return;
    };
    defer platform_dir.close();

    // 添加包含路径
    const include_base = std.fmt.allocPrint(b.allocator, "{s}/include", .{platform_path}) catch unreachable;

    // 检查并添加各个库的包含路径 (跳过cjson，因为主目录已有)
    const include_dirs = [_][]const u8{ "curl", "libubox", "modbus" };
    for (include_dirs) |include_dir| {
        const full_include_path = std.fmt.allocPrint(b.allocator, "{s}/{s}", .{ include_base, include_dir }) catch unreachable;
        if (std.fs.cwd().access(full_include_path, .{})) |_| {
            exe.addIncludePath(b.path(full_include_path));
        } else |_| {
            std.log.warn("Include directory {s} not found", .{full_include_path});
        }
    }

    // 添加uci.h的包含路径
    const uci_include_path = std.fmt.allocPrint(b.allocator, "{s}/uci.h", .{include_base}) catch unreachable;
    if (std.fs.cwd().access(uci_include_path, .{})) |_| {
        exe.addIncludePath(b.path(include_base));
    } else |_| {
        std.log.warn("UCI include path {s} not found", .{uci_include_path});
    }

    // 检查库目录是否存在
    const lib_path = std.fmt.allocPrint(b.allocator, "{s}/lib", .{platform_path}) catch unreachable;
    var lib_dir = std.fs.cwd().openDir(lib_path, .{}) catch |err| {
        if (err == error.FileNotFound) {
            std.log.warn("Library directory {s} not found, skipping library linking", .{lib_path});
            return;
        }
        std.log.err("Failed to open library directory {s}: {}", .{ lib_path, err });
        return;
    };
    defer lib_dir.close();

    // 设置库路径
    const library_path = b.path(lib_path);
    exe.addLibraryPath(library_path);

    // 定义需要链接的库
    const libraries = [_][]const u8{
        "cjson",
        "curl",
        //"modbus",
        "ubox",
        "ubus",
        "uci",
    };

    // 链接系统库
    for (libraries) |lib_name| {
        exe.linkSystemLibrary(lib_name);
        std.log.info("Linked system library: {s}", .{lib_name});
    }

    // 为交叉编译添加必要的链接选项
    exe.linkLibC();

    std.log.info("Cross-compiling to {s}, linked system libraries with library path", .{platform_name});
}
