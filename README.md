# skynetjit

在 Windows x64 与 Linux 上构建带 LuaJIT 的 Skynet。上游
[cloudwu/skynet](https://github.com/cloudwu/skynet) 通过
[dpull/skynet-mingw](https://github.com/dpull/skynet-mingw) 的嵌套子模块引入
（skynet-mingw 目前只作为子模块宿主，构建不使用它自带的代码），LuaJIT 使用
[OpenResty LuaJIT2](https://github.com/openresty/luajit2)。Windows 适配由本项目的
`compat/win64` 与上游 Skynet 自带的 `3rd/compat-mingw`（wepoll、dlfcn）提供。

这些上游项目都以 Git 子模块保存，本项目不会直接修改它们的工作树。构建时会用
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

## Linux 构建

Linux 使用独立的一键脚本，产物位于 `build/linux-out`，不与 Windows
`build/out` 混用。需要原生 Linux GCC、make、Git 和常用开发头文件：

```bash
./build-linux.sh
```

默认后端是原生 epoll，也是唯一后端：Linux/WSL 用 epoll，Windows 用 wepoll 兼容层。
（曾经有过一个可选的 io_uring 完成队列驱动后端，连同 `--backend` 选项一起移除了。）

常用选项：

- `--offline`：不访问远端，使用当前子模块提交；
- `--no-sync`：不检查或切换子模块，直接使用当前已检出的提交；适合 WSL 与
  Windows 共享同一工作区时的本地验证；
- `--no-test`：只编译，不执行测试；
- `--no-netbull`：跳过 `luaclib-src/` 的 7 个 NetBull Lua C 库（cjson、luacurl、
  lfs、pb、sqlite3、zlib、zset）及其 Lua 叠加文件；Windows 下同时不下载、不编译
  zlib/libcurl/CMake 依赖，Linux 下跳过 libcurl/zlib 开发包检查。默认编译这些库；
- `--no-redislib`：跳过 `redislib-src/` 的 Redis 8.10.0 算法内核与 4 个 Lua C 模块
  （bitmap、hll、sortedset、geo），不产出内核共享库 `rlcore` 与 `luaclib/redis/*.so`，
  也不叠加 `lualib/{bitmap,hll,sortedset,geo}.lua`，一键测试里的 redislib 冒烟同样跳过。
  默认编译，可用 `--redislib` 显式恢复；
- `--jobs N`：设置并行编译任务数。

也可以只更新源码：

```bash
./scripts/update-submodules.sh
```

## 持续集成

`.github/workflows/build.yml` 在 push、pull request 和手动触发时跑三个作业：

- `linux (full|minimal)`：Ubuntu 24.04 上 `./build-linux.sh --no-sync`，
  含一键测试；`minimal` 档加 `--no-netbull --no-redislib`，验证这两组确实可摘，
  摘掉后主干仍然构建并通过测试；
- `windows (full|minimal)`：Windows runner 上装 MSYS2 的 mingw-w64 x64 工具链，
  用仓库自己的入口 `build.bat` 跑同样的两档；`full` 档的 zlib/libcurl/CMake
  依赖缓存到 `build/deps`；
- `redis-diff`：下载并编译与移植目标同版本的 `redis-server` 8.10.0，先用
  `--no-test` 构建 redislib，再跑 `tools/redis-diff/run.sh`（两个种子的差分重放）
  与 `tools/redis-diff/interop.sh`（dump/load 互操作）。差分记录数不为 0 就判失败。

子模块按仓库里 pinned 的提交图检出，所以 CI 测的一定是可复现的那一组提交。
本机构建经常把嵌套的 `skynet` 子模块拉到比 pinned 更新的提交；要让 CI 也测那个
修订，用手动触发里的 `skynet_rev` 输入显式指定。

## 构建产物

Windows 主要文件在 `build/out`：

- `skynet.exe`、`skynet.dll`；
- `luajit.exe`、`lua51.dll`；
- `cservice/*.so`；
- `luaclib/*.so`；
- `luaclib/redis/*.so`（redislib 的 4 个模块）与它们共用的内核库 `rlcore.dll`，
  以及 `lualib/` 中的同名包装文件；
- Skynet 的 `lualib`、`service` 和 `examples`。

Linux 对应文件在 `build/linux-out`，包括 `skynet`、`libskynet.so`、`luajit`、
`libluajit.so`、`cservice/*.so`、`luaclib/*.so`、redislib 的 `librlcore.so` 与
`luaclib/redis/*.so`，以及同一套 Lua 运行时文件。

从输出目录启动自己的配置，例如：

```bat
cd build\out
skynet.exe examples\config
```

## 实现说明

- 默认构建会同步 `skynet-mingw` 所引用的嵌套 `cloudwu/skynet` 的 `master`；使用
  `--offline` 时则严格使用外层子模块记录的嵌套提交，适用于无网络的可复现构建。

- `compat/win64` 提供 Win64 `SOCKET`/`HANDLE` 到 Skynet 整型描述符的线程安全映射，
  并封装 socket、pipe、select 和 wepoll 调用，避免在 x64 下截断句柄；控制台 stdin
  通过读取线程和 loopback socketpair 接入 wepoll。
- Windows 构建将 WinSock `FD_SETSIZE` 统一设为 65535；Skynet socket 事件循环使用
  wepoll，且一键测试会同时创建并注册 65535 个 UDP socket 验证容量。
- Linux 使用原生 epoll，Windows 使用 wepoll 兼容层；两条路径都没有可选后端。补丁与
  叠加文件只作用在一次性解包目录（Windows `build/work`、Linux `build/linux-work`），
  不会修改子模块。
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
- `luaclib-src/` 收录原 NetBull 的 7 个 Lua C 库（cjson、luacurl、lfs、pb、
  sqlite3、zlib、zset/skiplist）源码，作为本仓库的 vendored 目录参与双平台构建，
  产物为 `luaclib/*.so`；`zset.lua` 与 `protoc.lua` 会叠加到输出的 `lualib/`。
  这些库不修改任何子模块，可用 `--no-netbull` 整组关闭。
- `redislib-src/` 把 Redis 8.10.0 的 Bitmap、HyperLogLog、Sorted Set、Geo 四组算法
  内核移植成进程内 Lua 库，命令面共 53 个：bitmap 7、hll 5、sortedset 31（该版本
  全部非阻塞 `Z*` 去掉 `ZSCAN`）、geo 10。内核文件只做三种机械编辑——include 换成
  本仓库的 shim、删掉 `xxxCommand(client*)` 命令层、加一行移植说明——`sds`、`dict`、
  `listpack`、`intset`、`adlist`、`t_zset`、`hyperloglog`、`bitops`、`geo`、
  `geohash*`、`pqsort`、`util`、`crc16` 等的算法行不动。内核只编译成一份共享库
  （Windows `rlcore.dll`、Linux `librlcore.so`），4 个模块都从这里取内核调用，不再
  各带一份内核副本——顺带也不再各带一份 `server`、dict 种子与 RNG 流。跨这个边界的
  只有函数：facade 原先直读的三个全局 `server`、`hashDictType`、`sdsReplyDictType`
  改走 `redislib_port.c` 里的访问函数，因为在 Windows 上取另一个 DLL 里数据的地址
  要靠链接器的 auto-import 改写，而不是导入表。`redislib-src/NOTICE` 逐节
  记录文件映射、shim 符号清单、与真 Redis 的偏差清单，以及许可依据：本移植按
  **内部自用**推进（Redis 源码为 AGPLv3/RSALv2/SSPLv1），若要对外分发或托管需
  重新评估。
- redislib 的业务入口是 `require "bitmap"`／`"hll"`／`"sortedset"`／`"geo"`，C 模块
  在子命名空间 `redis.<name>`（`luaclib/redis/*.so`）。方法名就是 Redis 命令名的小写，
  包装层另给大写别名（`z:ZADD`）与表形态入参（`z:zadd(1,"a",2,"b")` 与
  `z:zadd({{1,"a"},{2,"b"}})` 等价、`z:zrange(0,-1,{withscores=true,pairs=true})`）。
  返回值按 RESP 回包结构映射，Redis 的 nil 回复对应 `false`，错误用 Redis 客户端
  实际读到的文本（`ERR syntax error`、`WRONGTYPE …`）。对象是 handle，不是 key：
  没有 keyspace，`obj:dump()` 取值字节、`Mod.load(str)` 装回来，可与真 Redis 双向搬运
  （bitmap/hll 与 `GET`/`SET` 逐字节一致，sortedset/geo 用非 RDB 的小端简单格式）。
- Windows 构建会先由 `scripts/fetch-deps.sh` 把 zlib 与 libcurl 源码下载到
  `build/deps` 缓存，并用便携版 CMake 静态编译（libcurl 启用 Schannel HTTPS 与
  zlib），随后整体链接进 `luacurl.so` 和 `zlib.so`，输出目录不需要额外 DLL。
  `--offline` 时若缓存不存在会直接报错提示先联网构建一次。
- Linux 构建的 `luacurl.so`/`zlib.so` 直接链接系统 libcurl 与 zlib，需要
  `libcurl4-openssl-dev` 和 `zlib1g-dev`；`build-linux.sh` 会提前检查并给出提示。
- 分配与内存统计：上游把 `skynet_malloc` 等七个入口直接宏到 CRT `malloc`（这份
  fork 里连 `skynet_malloc.c` 都没有），于是 `malloc_hook.c` 的 cookie 与按服务记账
  整条编译不出去。实测就是：`skynet.memory` 的 `total`/`current`/`block`、
  `debug_console` 的 `mem`、launcher 的 `MEM` 一律读 0，哪怕手里正握着一条 4 MB 的
  消息缓冲区。默认构建（`ALLOC_HOOKS=1`）用 `patches/skynet-luajit.patch` 的一节让
  宏让位给真函数，并把 `malloc_hook.c` 中原本只服务于 jemalloc 的那一支的 `je_*`
  映射到 `compat/common/skynetjit_alloc.h` 的七个后端宏——同一套 cookie 与计数器直接
  复用，换分配器只需要改那一个文件。`NOUSE_JEMALLOC` 照旧生效，所以仍然不需要
  jemalloc 子模块。`--no-alloc-hooks` 回到上游那份宏的样子。后端为 libc 时
  `skynet.memory` 的 `jestat`/`dumpinfo`/`dumpheap`/`profactive` 读作"不可用"，
  它们依赖 jemalloc 的 `mallctl`。Lua VM 的分配走 `skynet_lalloc`（`realloc`/`free`），
  它一直不是宏、也一直不记账，这是上游既有语义，本仓库没有改。
  实测（WSL Ubuntu 24.04，默认构建）：启动后 `total=11149707 B`、`block=56`；4 MB
  消息在接收服务的 handler 内 `current=4206628 B`，返回后落回 17 B；`info()` 从
  0 项变成 8 项、合计 10888 KB。Windows 侧同一条探针给出逐字节相同的结果，
  rlcore 里那个 12288 字节的稠密 HLL 也被算进当前服务的 `live=12380 B`，
  说明跨模块（facade → rlcore → libskynet）的分配同样接上了。

  记账不是免费的。`tests/alloc_bench.lua` 单次运行的 `pack`/`trash`（一次
  `skynet_malloc`/`skynet_free` 往返）吞吐：

  | | 开钩（默认） | `--no-alloc-hooks` |
  | --- | --- | --- |
  | Linux，4 KB 消息 | 348k–364k ops/s | 603k ops/s |
  | Linux，64 B 消息 | 7.7M–8.8M ops/s | 12.3M ops/s |
  | Windows，4 KB 消息 | 241k–276k ops/s | 566k ops/s |
  | Windows，64 B 消息 | 4.2M–4.7M ops/s | 7.9M ops/s |

  即消息路径大约慢 1.7x（Linux）到 2x（Windows）；换成绝对值，一条 4 KB 消息
  的打包加释放是 2.9 µs 而不是 1.7 µs。其余几条循环在这台机器上两次运行能差
  30%，不足以支撑任何结论——要在目标机上重测再看。
- 对齐分配的三个入口（`skynet_memalign`、`skynet_aligned_alloc`、
  `skynet_posix_memalign`）在本仓库没有任何调用点（`skynet-src`、`service-src`、
  `lualib-src`、`redislib-src`、`luaclib-src` 全 grep 过，只剩声明与定义）。Linux 上
  它们映射到 `posix_memalign`（其结果本来就能 `free`）；Windows 上 CRT 只有
  `_aligned_malloc`，而它必须配 `_aligned_free`，接不进唯一的 `skynet_free`，所以
  调用即显式中止，而不是留一个哪天会踩的假实现。
- 分配器基准：`tests/alloc_bench.lua` + `tests/alloc-bench-config.lua`，量三条路——
  `pack`/`trash`（一次 `skynet_malloc`/`skynet_free` 往返）、VM 侧表分配、rlcore 的
  HLL 分配。它不进一键测试（对调度噪声敏感），比的是同一台机器上不同构建的相对差值：

  ```bash
  ./build-linux.sh && cd build/linux-out && ./skynet ../../tests/alloc-bench-config.lua
  ./build-linux.sh --no-alloc-hooks && cd build/linux-out && ./skynet ../../tests/alloc-bench-config.lua
  ```

一键测试会验证 LuaJIT 环境、核心 Lua C 模块、所有 Lua 文件语法，并真正启动
Skynet，验证 sharetable 的循环图、重复引用、函数、lightuserdata、只读保护和热更新，
再完成监听 socket 与 `skynet.abort` 正常退出。另有一项 NetBull luaclib 冒烟测试，
在 Skynet 服务内验证 cjson JSON 往返、lfs 目录属性、zlib 压缩往返（走
`skynet_malloc`）、内存 SQLite 增删查、zset 有序集合排名、luacurl 句柄与选项、以及
protoc+pb 的 protobuf 编码解码往返。还有一项 redislib 冒烟测试，同样在 Skynet
服务内跑 53 个命令的回复形状与 Redis 原文错误文本、`PFSELFTEST`、HLL 稀疏↔稠密往返、
有序集合的两种内部编码、以 Redis GEO 文档数值为准的地理距离与跨 180° 经线行为、
`dump()`/`load()` 字节互通、包装层的大写别名与表形态入参，以及 1e6 元素规模下的
rank/range/LIMIT 精确断言；Windows 实测整套 2.3 秒，`--no-redislib` 时该项打印跳过
说明并通过。Windows 额外验证 console stdin 命令和
wepoll 65535-socket 容量（可用 `SKYNETJIT_SOCKET_CAPACITY` 调整，上限 65535）；Linux
运行同一套运行时与退出测试，容量回归不在 Linux 上跑——那个回归针对的是 wepoll 在
WSAPoll 之上的 fd 上限，epoll 没有这个问题。

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
- redislib 移植的是 Redis 的**数据结构与算法**，不是它的服务器：没有 keyspace、
  DB、TTL、主从复制与 AOF，因此也没有 `SCAN`/`ZSCAN` 与阻塞族
  （`BZPOPMIN`/`BZPOPMAX`/`BZMPOP`）；对象是 Lua handle，命令打在 handle 上。
  `dump()`/`load()` 中只有 bitmap 与 hll 的字节等于真 Redis 的 `GET`/`SET` 值，
  sortedset 与 geo 用的是本项目 §4 定义的非 RDB 小端格式。逐条偏差见
  `redislib-src/NOTICE` 第 6 节。
  对目标是 **Redis 8.10.0**：`BITOP DIFF/DIFF1/ANDOR/ONE` 与 GEOSEARCH 的 `ASC`
  排序在 7.x 服务器上行为不同，拿 7.0.12 做同日志对比时这几处必然不一致（差异
  全部归因于版本，实测记录见 NOTICE 第 5 节）。另外 LuaJIT 是 5.1 数值模型，
  `bitfield` 的宽类型超过 2^53 时读写两侧都会在 Lua 层舍入（内核位布局精确，
  可用 `dump()` 字节核对）。
- 上游最新代码将来如果改变被补丁覆盖的上下文，构建会立即失败并提示补丁未应用，
  需要同步更新本仓库兼容层。
