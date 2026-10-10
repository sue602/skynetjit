# redislib 设计文档：Bitmap / Geo / HyperLogLog / SortedSet 的 Redis 兼容 Lua 库

日期：2026-10-10
状态：已与需求方逐节确认设计，待审阅本文档
来源：移植 Redis 8.10.0（本地 `D:\download\redis-8.10.0\src`）C 源文件

## 0. 决策记录（为什么是这个方案）

- 目标：功能与 Redis 一致——同算法、同编码、同返回语义，最终要能逐字节对测。
- 因此选择**直接移植 Redis 8.10.0 源文件**（而非用同算法重写），只删除命令/协议层。
- 许可：Redis 8.x 为 RSALv2 / SSPLv1 / AGPLv3 三许可。需求方确认**仅公司内部使用**，
  不分发、不对外托管；随附 `NOTICE` 与各源文件原有版权头。若未来分发形态变化，
  须重新做许可评估。
- 命名：新排序集模块命名 `sortedset.lua`；既有 `luaclib-src/lua/zset.lua`
  （NetBull skiplist 封装）**保持不动**。不使用 `ZSet.lua` 之类仅大小写不同的名字——
  Windows NTFS 大小写不敏感会与 `zset.lua` 互相劫持，Linux 又大小写敏感，会造成
  跨平台行为分裂。

## 1. 目录与产物

```
redislib-src/
  NOTICE                      # 来源、版本、三许可、内部使用声明
  kernel/                     # 自 Redis 8.10.0 原样搬运的算法文件
    geohash.c geohash.h
    geohash_helper.c geohash_helper.h
    hyperloglog.c             # 去命令层，保留 hll* 内核函数
    bitops.c                  # 只取位运算内核（popcount/bitfield/bitpos/bitcount）
    t_zset.c                  # 只取 zsl*/zset 内核（跳表+字典有序集）
    dict.c dict.h siphash.c
    sds.c sds.h sdsalloc.h
    adlist.c adlist.h
    util.c util.h
    fast_float_strtod.c fast_float_strtod.h
    intset.c intset.h
    endianconv.h monotonic.h (按实际引用裁剪)
    redislib_port.h redislib_port.c   # 本库唯一新增的“内核”文件（shim，见 §2）
  facade/
    rl_common.c rl_common.h   # userdata 对象模型、返回值映射、别名表
    bitmap.c geo.c hll.c sortedset.c  # 四个 luaopen_*，命令参数解析在此重写
    geo_sem.c                 # 从 geo.c 命令层剥出的 GEO* 语义 helper（见 §2.4）
  lua/
    bitmap.lua geo.lua hll.lua sortedset.lua   # 薄封装：大写命令名别名 + 文档
tools/redis-diff/             # 差分对测 harness（§6.2）
```

构建产物：`luaclib/{bitmap,geo,hll,sortedset}.so`（双平台）与
`lualib/{bitmap,geo,hll,sortedset}.lua`。

## 2. 内核移植规则

### 2.1 总原则
- 搬运文件**只做三类机械修改**，不改任何算法逻辑：
  1. `#include "server.h"` → `#include "redislib_port.h"`；
  2. 删除所有 `void xxxCommand(client *c)` 及其专属静态辅助（client/robj/keyspace
     引用整体消失）；
  3. 被删函数中确有算法价值的部分按 §2.4 移入 `geo_sem.c` 等 helper。
- 每个搬运文件头部保留 Redis 原版权注释，追加一行
  `/* skynetjit port: command layer removed, see redislib-src/NOTICE */`。

### 2.2 shim（redislib_port.h/.c）
- `zmalloc/zcalloc/zrealloc/zfree/s_malloc` → `skynet_malloc/skynet_calloc/
  skynet_realloc/skynet_free`（纳入项目内存监控；facade 的 .so 链接 `-lskynet`，
  Linux 带 `$ORIGIN/..` rpath，同 netbull zlib 模式）。
- `serverAssert/serverAssertionDisabled` → `assert`；
  `serverPanic/serverLog` → `luaL_error` 或直接 `abort`（仅不可达路径）。
- dict 初始化需要哈希种子（Redis 在启动时用随机字节）：`redislib_port.c` 提供
  一次性初始化的 `dict_hash_function_seed`，用单调时钟混合指针熵生成；行为对
  正确性无影响（dict 非加密用途，与 Redis 一致）。
- `RAND_MAX/random()`：HLL 内核不含随机（已核实无 lzf/mt19937 依赖）；
  如编译发现引用，用 xorshift 补位并注释。
- 需要但不在闭包里的 Redis 全局（`shared.*`）：逐个用局部静态常量替换
  （如 `shared.emptyarray` 这类仅命令层用到——删命令层后自然消失）。

### 2.3 各内核文件移植范围
- `hyperloglog.c`：保留 `hllCreate/hllAdd/hllCount/hllMerge/hllSparseToDense/
  hllSparseSet/hllDenseSet/...` 全族 + PFDEBUG 数据侧实现（Redis 仅有
  sparse→dense 单向转换）。删：`pfaddCommand` 等与 `genericHllTableCommand`。
- `bitops.c`：保留 `popcount*` 系列、`bitfieldComputeString`/`bitfieldGetOp` 族、
  `rangeLongLongBitCount`/`bitposCommand` 的扫描内核（`(bitcount 参数)` 语义在
  facade 里解析后调用内核）。
- `t_zset.c`：保留 `zslCreate/zslInsert/zslDelete/zslGetScore/zslFirstInRange/
  zslLastInRange/zslNthInRange/zslFree...` 与 `zset*`（基于 dict+zsl 的
  `zadd/zscore/zpopmax 内核`）。删：全部 `*Command` 与
  `genericZdiffCommand/genericZstoreCommand` 这类 client 胶水（其算法在
  facade 重写为对内核的调用）。
- `geo.c`：`extractDistance/extractRadius/geohashBitsCommand` 等 helper 全部移入
  `facade/geo_sem.c`；GEOADD/GEODIST 等语义在 facade 中按 `geo.c` 原文逐行改写
  （成员 sds 编解码走内核）。

### 2.4 唯一“重写”的部分
命令层（参数解析、RESP 回复组装、client 生命周期）不进库。facade 以
**相同的解析顺序与错误字符串**重新实现参数处理；错误消息文本对齐 Redis
（如 `"value is out of range"`），便于业务从真 Redis 迁移时无感。

## 3. Lua 接口

### 3.1 对象模型
- **类型原则（已确认）**：Lua 层对外只出现 Lua 原生类型——string / number /
  boolean / table（`false` 表示 Redis nil 回复），错误走 Lua error。唯一的
  userdata 是对象句柄本身，业务不触其内部。
- `local x = hll.new()` / `geo.new()` / `bitmap.new()` / `sortedset.new()`。
- full userdata + `__gc` + 显式 `free()`；不支持跨 Lua state 传递
  （skynet 服务间如需搬运，用 `dump()/load()`）。
- 方法名：Redis 命令名小写（`obj:pfadd("a")`）；`lua/*.lua` 层提供大写别名表
  （`obj:PFADD` 等价），业务从 redis-lua 迁移时代码改动最小。
- 参数约定：与 Redis 命令逐位对应；多 key 命令（PFMERGE、ZUNION…）接受多个
  同型对象句柄。`GEORADIUS ... STORE <key>` → `store = <geo对象>` 具名参数。
- 输入双形态（Lua 包装层归一化，C 层只实现变参一种）：
  `zs:add(1, "a", 2, "b")` 与 `zs:add({ {1,"a"}, {2,"b"} })` 等价。

### 3.2 返回值映射（写死并测）
- 结构原则（已确认）：**按 RESP 回包结构忠实映射**，不自创结构——业务在
  "本地库 ↔ 真 Redis 客户端"间切换零改动，§6.2 差分对测为逐字段相等。
  `sortedset.lua` 包装层可选提供 `pairs = true` 语法糖（返回
  `{{member=..., score=...}, ...}`），纯 Lua 实现，不影响 C 层忠实性。
| Redis 回复 | Lua |
|---|---|
| 整数 | number |
| 状态文本 ("OK"/"QUEUED") | 字符串 `"OK"` |
| 双精度数 (addReplyDouble：ZSCORE/ZINCRBY/GEODIST/WITHSCORES) | number |
| Bulk 可能是 nil | string 或 `false` |
| 数组 | table（保序保嵌套；`WITHSCORES` 为平铺数组 `[m1,s1,m2,s2...]`） |
| Error | `lua_error`（消息文本同 Redis） |

### 3.3 命令面（完整清单）
- **bitmap (7)**：setbit getbit bitcount(start/end 与 BYTE|BIT) bitpos(bit 值 参数)
  bitop(and/or/xor/not/diff/diff1/andor/one — 8 种操作，对照 8.10.0 bitopCommand)
  bitfield(GET|SET|INCRBY|OVERFLOW WRAP|SAT|FAIL，
  类型 iN/uN 全支持含 i64) bitfield_ro
- **geo (10)**：geoadd(NX XX CH) geopos geodist(米制单位四种) geohash
  georadius georadius_ro georadiusbymember georadiusbymember_ro
  (BYRADIUS/BYMEMBER/BYBOX 三形态 × WITHCOORD/WITHDIST/WITHHASH/COUNT+ANY/
  ASC/DESC/STORE/STOREDIST) geosearch(FROMMEMBER/FROMLONLAT × BYRADIUS/BYBOX
  × 同上回复选项) geosearchstore。
  注：本 8.10.0 源码树经检索**不含** GEOSHAPE/多边形(BYPOLYGON)地理形状索引
  功能，不在本期范围。
- **hll (5)**：pfadd pfcount(单/多对象) pfmerge pfdebug(8.10.0 实际共 5 子命令：
  `simd on|off` / `getreg` / `decode` / `encoding` / `todense`，已对照源码
  pfdebugCommand 核实；`simd` 在本库按编译目标固定报告 on/off) pfselftest
- **sortedset (31)**：zadd(NX XX GT LT CH INCR) zcard zcount zdiff zdiffstore
  zincrby zinter zintercard zinterstore zlexcount zmpop(COUNT/MIN|MAX，无 BLOCK)
  zmscore zpopmax zpopmin zrandmember(COUNT WITHSCORES) zrange(BYSCORE/BYLEX/
  REV/LIMIT offset count/WITHSCORES——8.10.0 的 ZRANGE 只有这一套过滤器，
  **没有** 9.x 的 BYRANK/BYINDEX 与 ZMIN/ZMAX 形态，已按本源码树核实)
  zrangebylex zrangebyscore
  zrangestore zrank(WITHSCORE) zrem zremrangebylex zremrangebyrank zremrangebyscore
  zrevrange zrevrangebylex zrevrangebyscore zrevrank zscore zunion zunionstore
  （计数说明：Redis 的 32 个 Z* 命令去掉 ZSCAN 正好 31 个。）

**明确不提供**：ZSCAN/SCAN 游标类（库内对象全量即得）、BLOCK 家族、
keyspace/DB、RESP 协议、RDB、cluster。文档里逐条列出，避免误用。

## 4. 持久化与真 Redis 互通
- `bitmap:dump()` → 字符串 = Redis string 值本体（GET 同键逐字节一致）。
- `hll:dump()` → HLL 字节串，与真 Redis `GET` 逐字节一致（sparse/dense 原样）。
  `hll.load(s)` 校验合法性同 Redis 读入检查。
- `sortedset:dump()/geo:dump()` → 自定义简单格式 `uint32 count + (double score,
  uint32 len, bytes member)*`（非 RDB；跨语言可解析，可灌回真 Redis：
  ZADD/GEOADD 逐条恢复）。`load` 对应。
- 目的：业务可把 blob 存进任意介质（DB/文件/skynet sharedata），并与真 Redis
  双向搬运 HLL/bitmap。

## 5. 构建集成
- `integration/Makefile`（Windows）与 `integration/linux.mk` 各加一段
  `redislib.mk` include（模式照抄 `netbull.mk`：共享规则 + 平台变量）。
- `scripts/build-common.sh`：新选项 `--redislib / --no-redislib`，默认开；
  关闭时：不编 4 个 .so、不叠加 `redislib-src/lua/*.lua`、跳过其冒烟测试
  （检测 `luaclib/sortedset.so` 存在与否，同 clibs 测试现有做法）。
- `build.sh / build-linux.sh / build.bat`：透传（build.bat 已原样透传 %*）。
- 无新增外部依赖/下载。

## 6. 测试
### 6.1 冒烟（并入一键测试，双平台）
`tests/redislib-config.lua + tests/redislib_smoke.lua`，按 netbull clibs 模式：
- 每命令至少 1 正例 + 边界：空对象、单元素、BITFIELD 三种 OVERFLOW 与 i64/u63
  边界、bitcount 的 BYTE|BIT、HLL sparse→dense 转换阈值（Redis 默认
  `hll_sparse_max_bytes=3000`，库暴露同名可调项）、
  pfcount 相对误差断言（Redis 官方 0.81%）、sortedset 的 ±inf/NaN/lex 无穷、
  GEO 跨 180° 经线、count 1e6 元素 zrange 正确性（性能不设硬指标）。
- `pfselftest` 直接跑（内核自带实现，即最强自证）。
- 错误路径：错误消息文本断言（与 Redis 字符串一致）。

### 6.2 差分对测（tools/redis-diff，手动跑）
- WSL Ubuntu-24.04 内 `make -C /mnt/e/... redis-8.10.0` 构建真 redis-server。
- `gen.lua <seed>` 生成确定性操作序列（同一 seed 双方重放）。
- `replay_redis.py`（标准库 socket 手写 RESP，无第三方依赖）驱动真 Redis；
  `replay_lib.lua`（skynet 服务）驱动本库；两边导出
  `命令返回值 + dump 字节串` 到 JSONL，`diff.py` 逐条比对。
- 覆盖：三组全命令 + 互通 load（Redis `SET` 一个 hll blob → 本库 load）。
- 该 harness 不进默认测试；CI 化以后再说。

## 7. 风险与对策
| 风险 | 对策 |
|---|---|
| `t_zset.c`/`dict.c` 内部引用散布（kvstore、zmalloc 变体） | 以“删命令层”为手术边界，编译报错逐个 shim；搬运清单已按 §2 收敛 |
| MinGW 与 glibc 的 long double/double 打印差异影响 GEO 距离字符串 | GEO 距离返回 number 而非 RESP 字符串；差分对测比数值（容差 1e-9） |
| HLL 估计值是否依赖哈希种子 | 不依赖：dict 才用 siphash，HLL 用文件内联的 MurmurHash64A（hashFunction），已核实源码；差分对测覆盖 |
| dict/sds 更新节奏与上游脱钩 | NOTICE 钉死 8.10.0；升级需重新走差分对测 |
| 业务把 `geo.new()` 当 keyspace 用（全局单例假设） | README/包装层文档写明“对象即 key”模型 |

## 8. 交付拆分（实现计划的任务粒度）
1. 内核搬运 + shim 编译通过（产出 libkernel，命令行冒烟：PFSELFTEST）
2. facade: bitmap + 冒烟
3. facade: hll + 冒烟
4. facade: sortedset + 冒烟
5. facade: geo（含 geo_sem）+ 冒烟
6. dump/load 与互通格式
7. lua 包装层（大写别名）+ 文档
8. build 开关与双平台接线
9. 冒烟并入一键测试
10. redis-diff harness + WSL 真 Redis 对测报告

依赖：1 → 2..6；4 先于 5（geo 依赖 zset 对象）；8/9 最后合并验证；10 独立可并行。
