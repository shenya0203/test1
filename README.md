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
C:\zig\zig.exe build openwrt
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
