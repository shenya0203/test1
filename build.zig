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

const c_flags = [_][]const u8{
    "-Os", // 优化大小
    "-fdata-sections", // 将数据放在独立段，方便 linker 剔除
    "-ffunction-sections", // 将函数放在独立段，方便 linker 剔除
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // 全局调试选项
    const is_debug = b.option(bool, "debug", "Build with debug symbols") orelse false;

    // 版本标识位：bz=标准版，dz=定制版；仅在 `zig build version` 时使用
    const build_tag = b.option(
        []const u8,
        "build_tag",
        "Build tag identifier: bz (standard) or dz (custom), default bz",
    ) orelse "bz";

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
        createPlatformBuildStep(b, platform, mod, is_debug);
    }

    // 版本头文件按需刷新：`zig build version [-Dbuild_tag=bz|dz]`
    // 一次性重新生成所有平台的 hi_cfm_version_*.h
    const version_step = b.step(
        "version",
        "Regenerate version headers for all platforms (reads VERSION_BASE_VAL from Makefile, uses current system time)",
    );
    const version_gen = VersionStep.create(b, build_tag, &platforms);
    version_step.dependOn(&version_gen.step);

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

    // 添加包含路径以支持 @cImport
    exe.root_module.addIncludePath(b.path("src/hlk_cloud/src/include"));
    exe.root_module.addIncludePath(b.path("src/hlk_cloud/src/source"));
    exe.root_module.addIncludePath(b.path("src/hlk_cloud/src"));

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
fn createPlatformBuildStep(b: *std.Build, platform: PlatformConfig, mod: *std.Build.Module, is_debug: bool) void {
    const step = b.step(platform.name, std.fmt.allocPrint(b.allocator, "Build for {s} ({s})", .{ platform.name, platform.product_id }) catch unreachable);

    // 注意：版本头文件 hi_cfm_version_{platform}.h 由独立的 `zig build version` 步骤负责刷新，
    // 这里不再参与生成，平台构建仅消费已存在的头文件。

    // 创建可执行文件
    const exe = b.addExecutable(.{
        .name = std.fmt.allocPrint(b.allocator, "hlk_cloud_{s}", .{platform.name}) catch unreachable,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = b.resolveTargetQuery(platform.arch),
            .optimize = if (is_debug) .Debug else .ReleaseSmall, // 调试时使用Debug优化
            .strip = !is_debug, // 调试时保留符号表
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

// ============================================================
// 版本头文件生成 Step
// 仅在显式运行 `zig build version` 时触发，生成/覆盖所有平台的
// hi_cfm_version_{platform}.h 文件。
//
// 版本字符串格式：
//     "{product_id}-{VERSION_BASE_VAL}-{build_tag}-{yyMMdd.HHmmss}"
//   例：
//     "7628-V1.0.1-bz-251222.233450"
// ============================================================
const VersionStep = struct {
    step: std.Build.Step,
    build_tag: []const u8,
    platforms: []PlatformConfig,

    const makefile_path = "src/hlk_cloud/Makefile";
    const header_dir = "src/hlk_cloud/src/include";

    pub fn create(b: *std.Build, build_tag: []const u8, platforms: []const PlatformConfig) *VersionStep {
        const self = b.allocator.create(VersionStep) catch @panic("OOM");
        const owned_platforms = b.allocator.dupe(PlatformConfig, platforms) catch @panic("OOM");
        self.* = .{
            .step = std.Build.Step.init(.{
                .id = .custom,
                .name = "regenerate version headers",
                .owner = b,
                .makeFn = make,
            }),
            .build_tag = build_tag,
            .platforms = owned_platforms,
        };
        return self;
    }

    fn make(step: *std.Build.Step, options: std.Build.Step.MakeOptions) anyerror!void {
        _ = options;
        const self: *VersionStep = @fieldParentPtr("step", step);
        const b = step.owner;
        const allocator = b.allocator;

        // 校验 build_tag 必须为 bz 或 dz
        if (!std.mem.eql(u8, self.build_tag, "bz") and !std.mem.eql(u8, self.build_tag, "dz")) {
            std.log.err("Invalid -Dbuild_tag='{s}', only 'bz' (standard) or 'dz' (custom) are allowed", .{self.build_tag});
            return error.InvalidBuildTag;
        }

        // 1. 从 Makefile 读取 VERSION_BASE_VAL
        const makefile_content = readFileToEndAlloc(allocator, makefile_path, 1 * 1024 * 1024) catch |err| {
            std.log.err("Failed to read {s}: {}", .{ makefile_path, err });
            return err;
        };
        defer allocator.free(makefile_content);

        const version_base = extractVersionBase(allocator, makefile_content) catch |err| {
            std.log.err("Failed to extract VERSION_BASE_VAL from {s}: {}", .{ makefile_path, err });
            return err;
        };
        defer allocator.free(version_base);

        // 2. 获取当前日期时间（yyMMdd.HHmmss）
        const date_str = try getCurrentDateTime(allocator);
        defer allocator.free(date_str);

        std.log.info(
            "Generating version headers: base={s}, tag={s}, time={s}",
            .{ version_base, self.build_tag, date_str },
        );

        // 3. 为每个平台写入 hi_cfm_version_{platform}.h
        for (self.platforms) |platform| {
            const file_path = try std.fmt.allocPrint(
                allocator,
                "{s}/hi_cfm_version_{s}.h",
                .{ header_dir, platform.name },
            );
            defer allocator.free(file_path);

            const content = try std.fmt.allocPrint(
                allocator,
                "#define AT_VERSION \"{s}-{s}-{s}-{s}\"\n",
                .{ platform.product_id, version_base, self.build_tag, date_str },
            );
            defer allocator.free(content);

            std.fs.cwd().writeFile(.{
                .sub_path = file_path,
                .data = content,
            }) catch |err| {
                std.log.err("Failed to write {s}: {}", .{ file_path, err });
                return err;
            };

            std.log.info("  -> {s}: {s}", .{ file_path, std.mem.trimRight(u8, content, "\r\n") });
        }
    }
};

// 读取整个文件到堆上分配的缓冲区。
// 使用最底层的 file.read 循环实现，避免依赖跨 Zig 版本变动频繁的高层 API：
//   - std.fs.Dir.readFileAlloc: 参数顺序/类型在 0.14→0.15 变过
//   - std.fs.File.readAll: 在 Zig 0.16-dev Io 重构中被移除
fn readFileToEndAlloc(allocator: std.mem.Allocator, path: []const u8, max_bytes: usize) ![]u8 {
    var file = try std.fs.cwd().openFile(path, .{});
    defer file.close();

    const stat = try file.stat();
    if (stat.size > max_bytes) return error.FileTooBig;

    const size: usize = @intCast(stat.size);
    const buf = try allocator.alloc(u8, size);
    errdefer allocator.free(buf);

    var pos: usize = 0;
    while (pos < size) {
        const n = try file.read(buf[pos..]);
        if (n == 0) return error.UnexpectedEndOfFile;
        pos += n;
    }
    return buf;
}

// 从 Makefile 文本中提取 `VERSION_BASE_VAL := <value>` 的值
// 支持 `:=`、`=`、`?=` 三种赋值形式，忽略行内注释（`#` 之后的内容）
fn extractVersionBase(allocator: std.mem.Allocator, content: []const u8) ![]u8 {
    const key = "VERSION_BASE_VAL";
    var iter = std.mem.splitScalar(u8, content, '\n');
    while (iter.next()) |raw_line| {
        const line = std.mem.trim(u8, raw_line, " \t\r");
        if (!std.mem.startsWith(u8, line, key)) continue;

        var rest = std.mem.trim(u8, line[key.len..], " \t");
        if (std.mem.startsWith(u8, rest, ":=")) {
            rest = rest[2..];
        } else if (std.mem.startsWith(u8, rest, "?=")) {
            rest = rest[2..];
        } else if (std.mem.startsWith(u8, rest, "=")) {
            rest = rest[1..];
        } else {
            continue;
        }

        // 去掉行内注释
        if (std.mem.indexOfScalar(u8, rest, '#')) |hash_idx| {
            rest = rest[0..hash_idx];
        }
        rest = std.mem.trim(u8, rest, " \t");
        if (rest.len == 0) continue;

        return try allocator.dupe(u8, rest);
    }
    return error.VersionBaseNotFound;
}

// 获取当前系统时间，格式 yyMMdd.HHmmss（例 251222.233450）
// Windows 使用 PowerShell Get-Date，其他系统使用 date 命令
fn getCurrentDateTime(allocator: std.mem.Allocator) ![]u8 {
    const argv: []const []const u8 = switch (builtin.os.tag) {
        .windows => &[_][]const u8{ "powershell", "-NoProfile", "-Command", "Get-Date -Format 'yyMMdd.HHmmss'" },
        else => &[_][]const u8{ "date", "+%y%m%d.%H%M%S" },
    };

    const result = std.process.Child.run(.{
        .allocator = allocator,
        .argv = argv,
    }) catch |err| {
        std.log.err("Failed to run datetime command: {}", .{err});
        return err;
    };
    defer allocator.free(result.stderr);
    errdefer allocator.free(result.stdout);

    if (result.term != .Exited or result.term.Exited != 0) {
        std.log.err(
            "Datetime command failed: term={any}, stderr={s}",
            .{ result.term, result.stderr },
        );
        allocator.free(result.stdout);
        return error.DatetimeCommandFailed;
    }

    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    const owned = try allocator.dupe(u8, trimmed);
    allocator.free(result.stdout);
    return owned;
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
        "-g", // 添加调试信息
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
        "src/hlk_cloud/src/modbus_collector", // Modbus收集器头文件目录
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
    addCSourcesFromDir(allocator, &c_files, "src/hlk_cloud/src/modbus_collector");
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
        // 【新增】这一行非常重要！让 Zig 的 @cImport 也能找到这些路径
        exe.addIncludePath(b.path(path));
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
    const include_dirs = [_][]const u8{ "curl", "libubox", "modbus", "openssl" };
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
        "ssl",
        "crypto",
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
