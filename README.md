# Zig OpenWrt Project

A proper Zig project configured for cross-compilation to OpenWrt routers (aarch64-linux-musl).

## Project Structure

```
.
├── src/
│   ├── main.zig          # Main application with Hello OpenWrt message
│   └── root.zig          # Library module
├── .vscode/
│   └── tasks.json        # VS Code build tasks
├── build.zig            # Build configuration with cross-compilation support
├── build.zig.zon        # Project metadata
└── zig-out/             # Build output directory
    └── openwrt/
        └── test1         # OpenWrt binary
```

## Build Commands

### 1. Project Initialization
```bash
# Initialize a generic executable project (already done)
zig init

# For future reference if starting from scratch:
zig init-exe
```

### 2. Build Commands

#### Build for Native Platform
```bash
# Using VS Code tasks (Ctrl+Shift+P → Tasks: Run Task → Build Native)
# Or directly:
C:\zig\zig.exe build
```

#### Build for OpenWrt (Multiple Methods)

**Method 1: Using dedicated build step (Recommended)**
```bash
# Using VS Code tasks (default build task)
# Or directly:
C:\zig\zig.exe build openwrt
```

**Method 2: Using target option**
```bash
C:\zig\zig.exe build -Dtarget=aarch64-linux-musl
```

**Method 3: Using custom flag**
```bash
C:\zig\zig.exe build -Dopenwrt=true
```

### 3. Run Commands

#### Run on Native Platform
```bash
# Using VS Code tasks (Ctrl+Shift+P → Tasks: Run Task → Run Native)
# Or directly:
C:\zig\zig.exe build run
```

#### Run Tests
```bash
# Using VS Code tasks (Ctrl+Shift+P → Tasks: Run Task → Test)
# Or directly:
C:\zig\zig.exe build test
```

## VS Code Integration

The project includes a `.vscode/tasks.json` file with pre-configured tasks:

- **Build OpenWrt** (default): Cross-compiles for OpenWrt automatically
- **Build Native**: Builds for the host platform
- **Build with Target Option**: Uses `-Dtarget=aarch64-linux-musl`
- **Build OpenWrt (Custom Flag)**: Uses `-Dopenwrt=true`
- **Run Native**: Builds and runs on host platform
- **Test**: Runs unit tests
- **Clean**: Cleans build cache

### Usage in VS Code

1. **Open Command Palette**: `Ctrl+Shift+P`
2. **Select Task**: `Tasks: Run Task`
3. **Choose**: Select any of the above tasks
4. **Default Build**: `Ctrl+Shift+B` runs "Build OpenWrt"

## Application Features

The `src/main.zig` application:
- Prints "Hello OpenWrt!" message
- Displays CPU architecture, OS, and ABI information
- Useful for verifying cross-compilation target

Example output on OpenWrt:
```
Hello OpenWrt!
CPU Architecture: aarch64
OS: linux
ABI: musl
Run `zig build test` to run the tests.
```

## Deployment to OpenWrt Router

### 1. Build the Binary
```bash
C:\zig\zig.exe build openwrt  #build for MT7981 SOC
C:\zig\zig.exe build mt7688   #build for MT7688 SOC
C:\zig\zig.exe build mt7621   #build for MT7621 SOC
```

### 2. Transfer to Router

**Using SCP (Replace with your router's IP):**
```bash
scp zig-out/openwrt/test1 root@192.168.1.1:/tmp/
```

**Alternative SCP commands:**
```bash
# If using different username:
scp zig-out/openwrt/test1 admin@192.168.1.1:/tmp/

# If SSH uses different port:
scp -P 2222 zig-out/openwrt/test1 root@192.168.1.1:/tmp/

# Copy with specific permissions:
scp zig-out/openwrt/test1 root@192.168.1.1:/tmp/ && ssh root@192.168.1.1 "chmod +x /tmp/test1"
```

### 3. Run on Router
```bash
ssh root@192.168.1.1
cd /tmp
./test1
```

## 版本头文件管理

项目中 C 代码使用的 `AT_VERSION` 宏由 `build.zig` 的 `version` 步骤按需生成，
**平台构建（`zig build mt7688` 等）不再自动刷新版本头文件**。

### 版本字符串格式

```
#define AT_VERSION "{产品型号}-{基础版本号}-{标识}-{日期时间}"
```

| 段位 | 来源 | 示例 |
|---|---|---|
| 产品型号 | `build.zig` 中每个 `PlatformConfig.product_id` | `7628` / `RM65` / `RM60` |
| 基础版本号 | `src/hlk_cloud/Makefile` 的 `VERSION_BASE_VAL` | `V1.0.1` |
| 标识 | `-Dbuild_tag` 命令行参数（默认 `bz`） | `bz`（标准） / `dz`（定制） |
| 日期时间 | 构建机当前系统时间，格式 `yyMMdd.HHmmss` | `251222.233450` |

最终生成示例：

```c
#define AT_VERSION "7628-V1.0.1-bz-251222.233450"
```

### 刷新版本头文件

```bash
# 生成标准版（默认 build_tag=bz）
zig build version

# 生成定制版
zig build version -Dbuild_tag=dz
```

执行后会**一次性刷新三个平台的版本头文件**（会被 `.gitignore` 忽略）：

- `src/hlk_cloud/src/include/hi_cfm_version_mt7688.h`
- `src/hlk_cloud/src/include/hi_cfm_version_mt7981.h`
- `src/hlk_cloud/src/include/hi_cfm_version_mt7621.h`

### 使用约束

- **新克隆仓库或执行 `zig build clean` 后**：必须先运行一次 `zig build version`，
  否则后续 `zig build mt7688` / `mt7981` / `mt7621` 会因找不到版本头文件而编译失败。
- **发版时**：在构建产物之前手动执行一次 `zig build version`（根据是标准版还是定制版
  传入 `-Dbuild_tag`），确保打包的二进制含最新时间戳。
- **日常增量构建**：不需要每次都刷新版本，保留上一次的头文件即可，避免无谓的全量重编。
- **基础版本号修改**：统一改 `src/hlk_cloud/Makefile` 里的 `VERSION_BASE_VAL`，
  然后 `zig build version` 会自动读取新值，**build.zig 不需要改**。
- **Windows 环境**下使用 PowerShell 获取时间；Linux 环境下会自动回退到 `date` 命令
  （当前主要在 Windows 下开发）。

### 注意事项

- 若 Makefile 中找不到 `VERSION_BASE_VAL`、或取系统时间失败，`version` 步骤会直接报错退出，
  **不提供回退/默认值**，需要开发者自行介入修复。
- `-Dbuild_tag` 只接受 `bz` 或 `dz`，其它值会报错。

## Build System Configuration

The `build.zig` file provides:

- **Standard target options**: Use `-Dtarget=` for any architecture
- **Custom OpenWrt flag**: Use `-Dopenwrt=true` for quick OpenWrt builds
- **Dedicated build step**: Use `zig build openwrt` for explicit OpenWrt builds
- **Module system**: Proper separation of concerns between main and library code

## Supported Targets

You can build for various targets using `-Dtarget=`:

```bash
# Common ARM targets
zig build -Dtarget=aarch64-linux-musl    # OpenWrt ARM64
zig build -Dtarget=arm-linux-musleabihf # OpenWrt ARM32
zig build -Dtarget=riscv64-linux-musl   # RISC-V 64-bit

# Other targets
zig build -Dtarget=x86_64-linux-gnu     # Standard Linux 64-bit
zig build -Dtarget=x86_64-windows     # Windows 64-bit
```

## Troubleshooting

### Build Issues
- Ensure Zig is in PATH or use full path: `C:\zig\zig.exe`
- Check target compatibility with your router's architecture
- Use `zig targets` to see all available targets

### Deployment Issues
- Verify router SSH access: `ssh root@<router-ip>`
- Check `/tmp` directory permissions
- Ensure binary has execute permissions: `chmod +x /tmp/test1`

### VS Code Issues
- Reload VS Code after creating `.vscode/tasks.json`
- Check that Zig Language Server (ZLS) is installed and configured

## Next Steps

1. **Test the build**: Run `C:\zig\zig.exe build openwrt`
2. **Deploy to router**: Use the scp command above
3. **Verify execution**: SSH into router and run the binary
4. **Develop**: Modify `src/main.zig` and rebuild as needed

Happy coding with Zig on OpenWrt! 🚀


# 使用repomix 打包工程给AI
npx repomix@latest --ignore ".zig-cache/,.zig-cache-new/,.vscode/,.zig-out/,src/hlk_cloud/MQTTPacket/,src/hlk_cloud/openwrt,src/hlk_cloud/platform/,src/include/,src/linux/,src/hlk_cloud/src/source/yyjson.c,src/hlk_cloud/src/include/yyjson.h,src/platform/mt7688/include/openssl/obj_mac.h,src/platform/mt7688/include/curl/curl.h,src/platform/mt7981/include/curl/curl.h,src/platform/mt7688/include/openssl/ssl.h,src/platform/mt7621/include/curl/curl.h,src/platform/mt7688/include/openssl/tls1.h,src/platform/mt7688/include/openssl/evp.h,src/hlk_cloud/src/source/cJSON.c,src/platform/mt7688/include/openssl/ec.h,src/platform/mt7688/include/openssl/x509.h,src/platform/mt7688/include/openssl/sslerr.h,src/platform/mt7688/include/openssl/asn1.h,src/platform/mt7688/include/openssl/,src/platform/mt7688/include/,src/platform/mt7981/,src/platform/mt7621/,src/hlk_cloud/src/source/hi_link_ipc.c,src/hlk_cloud/src/source/md5.c,src/hlk_cloud/src/client/MQTTClient.c,"