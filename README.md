# skynetjit

在 Windows x64 上将 [dpull/skynet-mingw](https://github.com/dpull/skynet-mingw)
的 Windows 适配层与 [OpenResty LuaJIT2](https://github.com/openresty/luajit2)
组合起来构建 Skynet。

两个上游项目都以 Git 子模块保存，本项目不会直接修改它们的工作树。构建时会用
`git archive` 生成一次性工作副本，再向工作副本应用兼容补丁，产物位于
`build/out`。

## 一键构建

机器上需要：

- Git for Windows；
- `C:\MinGW\msys\1.0` 或 MSYS2；
- x64 MinGW-w64 工具链。本机默认路径为 `C:\mingw64`，其
  `gcc -dumpmachine` 必须输出 `x86_64-w64-mingw32`。

在 CMD 中执行：

```bat
build.bat
```

脚本默认会先拉取 `skynet-mingw/master` 和
`luajit2/v2.1-agentzh` 的最新提交，然后初始化 `skynet-mingw` 自带的
`skynet` 子模块，最后完成构建与测试。

如果源码已经下载好，只做可复现的离线构建：

```bat
build.bat --offline
```

指定工具路径：

```bat
set BASH_EXE=C:\MinGW\msys\1.0\bin\bash.exe
set MINGW64_ROOT=C:\mingw64
build.bat
```

在 MSYS Bash 中可直接运行：

```bash
export MINGW64_ROOT=/c/mingw64
./build.sh
```

常用选项：

- `--offline`：不访问远端，使用当前子模块提交；
- `--no-test`：只编译，不执行测试；
- `--jobs N`：设置并行编译任务数。

也可以只更新源码：

```bash
./scripts/update-submodules.sh
```

## 构建产物

主要文件均在 `build/out`：

- `skynet.exe`、`skynet.dll`；
- `luajit.exe`、`lua51.dll`；
- `cservice/*.so`；
- `luaclib/*.so`；
- Skynet 的 `lualib`、`service` 和 `examples`。

从输出目录启动自己的配置，例如：

```bat
cd build\out
skynet.exe examples\config
```

## 实现说明

- `compat/win64` 提供 Win64 `SOCKET`/`HANDLE` 到 Skynet 整型描述符的线程安全映射，
  并封装 socket、pipe、select 和 wepoll 调用，避免在 x64 下截断句柄。
- `compat/luajit` 提供 Skynet 当前 Lua 5.4 C API 到 LuaJIT 2.1 API 的适配。
- `compat/lua` 补齐 Skynet 使用的 Lua 5.3/5.4 Lua 层接口与语法差异。
- `skynet.sharetable` 在 LuaJIT 下使用 Skynet `sharedata` 的进程级 C 堆作为后端，
  保留常用公开 API，并通过只读 table 代理实现跨 Lua State 共享与更新。
- `patches/skynet-luajit.patch` 只应用到 `build/work`，不会污染子模块。
- 构建使用 `NOUSE_JEMALLOC`，所以不需要初始化 Skynet 的 jemalloc 子模块。

一键测试会验证 PE/x64 LuaJIT 环境、核心 Lua C 模块、所有 Lua 文件语法，并真正
启动 Skynet，验证 sharetable 加载/查询/只读保护/热更新，再完成一次监听 socket
的创建、注册和关闭。

## 已知边界

- LuaJIT 的 `sharetable` 后端支持无环树、整数/字符串键以及
  number/string/boolean/table 值。它不支持循环或重复 table 引用、函数、
  lightuserdata，也不保证 `next/rawget/rawset` 与原生共享 Table 完全一致；应使用
  普通索引、`pairs`、`ipairs` 和长度运算符访问。
- LuaJIT 采用 Lua 5.1 数值模型。兼容层覆盖了 Skynet 当前源码所需接口，但依赖
  Lua 5.4 精确 64 位整数语义或 to-be-closed 变量的第三方业务代码仍需单独适配。
- 上游最新代码将来如果改变被补丁覆盖的上下文，构建会立即失败并提示补丁未应用，
  需要同步更新本仓库兼容层。
