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
  并封装 socket、pipe、select 和 wepoll 调用，避免在 x64 下截断句柄；控制台 stdin
  通过读取线程和 loopback socketpair 接入 wepoll。
- Windows 构建将 WinSock `FD_SETSIZE` 统一设为 8192；Skynet socket 事件循环使用
  wepoll，且一键测试会同时创建并注册 8192 个 UDP socket 验证容量。
- Win64 I/O 包装保持 POSIX 的零长度 read/recv 立即返回语义，使 `skynet.abort`
  能够处理无 payload 的退出控制命令并完成线程回收。
- `compat/luajit` 提供 Skynet 当前 Lua 5.4 C API 到 LuaJIT 2.1 API 的适配。
- `compat/lua` 补齐 Skynet 使用的 Lua 5.3/5.4 Lua 层接口与语法差异。
- `skynet.sharetable` 在 LuaJIT 下使用 Skynet `sharedata` 的进程级 C 堆作为后端，
  将任意 table 图扁平编码后，通过只读 table 代理实现跨 Lua State 共享与更新；
  不直接共享 LuaJIT 的 GC 对象。
- `compat/luajit/sharetable_bridge.c` 只负责进程内的 lightuserdata 和无 upvalue
  C function 转换；LuaJIT fast function 使用全局表或已加载模块中的符号引用重建。
- `patches/skynet-luajit.patch` 只应用到 `build/work`，不会污染子模块。
- 构建使用 `NOUSE_JEMALLOC`，所以不需要初始化 Skynet 的 jemalloc 子模块。

一键测试会验证 PE/x64 LuaJIT 环境、核心 Lua C 模块、所有 Lua 文件语法，并真正
启动 Skynet，验证 sharetable 的循环图、重复引用、函数、lightuserdata、只读保护
和热更新，再完成监听 socket、`skynet.abort` 正常退出、console stdin 命令及
wepoll 8192-socket 容量测试。

## 已知边界

- LuaJIT 的 `sharetable` 后端支持循环与重复 table 引用；number、string、boolean、
  table、lightuserdata 和 function 均可作为键或值。Lua 函数和 C 函数都必须没有
  upvalue，table 不能带 metatable；full userdata 和 thread 仍会被明确拒绝。
- 普通索引、长度运算符、`pairs`、`ipairs` 以及 Lua 层的 `next/rawget/rawset` 已兼容，
  其中 `rawset` 以及全局 `table.insert/remove/sort/move` 的写入会保持只读并报错。
  它本质上仍是代理，而不是 LuaJIT 内部的原生
  `Table *`；第三方 C 模块直接调用 `lua_next`、`lua_rawget`，或 LuaJIT 自带 table 库
  绕过元方法访问时，看不到代理的逻辑内容。此时可调用本项目扩展
  `sharetable.copy(proxy)`，获得保留循环和重复引用关系的普通可变 table 副本。
- 热更新通过稳定路径复用已取得的嵌套代理。对由字符串、数字或布尔字段可达的常规
  配置图可以稳定更新；如果某个对象只通过 table/function/lightuserdata 键可达，且
  更新同时改变了这类键的排序，先前单独持有的嵌套代理不保证还能映射到新对象。
- Lua bytecode 与普通 C function 入口仅用于同一进程、同一构建中的服务间传递，
  不能把内部图编码当成可落盘或跨进程交换的格式。
- LuaJIT 采用 Lua 5.1 数值模型。兼容层覆盖了 Skynet 当前源码所需接口，但依赖
  Lua 5.4 精确 64 位整数语义或 to-be-closed 变量的第三方业务代码仍需单独适配。
- 上游最新代码将来如果改变被补丁覆盖的上下文，构建会立即失败并提示补丁未应用，
  需要同步更新本仓库兼容层。
