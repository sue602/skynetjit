-- 分配器基准：把 skynet 自己的分配入口单独量出来，用于比较
-- --no-alloc-hooks（skynet_malloc 宏到 CRT malloc）与默认（走 malloc_hook.c 的
-- 真函数 + cookie 记账）两种构建，也用于比较换后端前后的差别。
--
-- 不进一键测试：这类循环对调度噪声敏感，只适合手动跑并看相对差值。
--
--   ./build-linux.sh && cd build/linux-out && ../../tests/... 见 README

local skynet = require "skynet"

local NS = 1000000000
local loaded, memory = pcall(require, "skynet.memory")

local function out(fmt, ...)
	io.stdout:write(string.format(fmt, ...))
	io.stdout:flush()
end

local function memline(tag)
	if not loaded then
		out("  %-12s skynet.memory 未载入\n", tag)
		return
	end
	out("  %-12s total=%d B  block=%d  live=%d B\n", tag,
		memory.total(), memory.block(), memory.current())
end

-- bytes_per_iter 只用于换算吞吐，量级对得上就行；循环本身的耗时才是比较对象。
local function bench(label, iters, bytes_per_iter, fn)
	local t0 = skynet.hpc()
	fn()
	local dt = (skynet.hpc() - t0) / NS
	out("%-26s %8.3f s  %12.0f ops/s  %10.1f MB/s\n",
		label, dt, iters / dt, iters * bytes_per_iter / dt / 1048576)
end

skynet.start(function()
	out("alloc-bench  (iterations are fixed; compare runs, not absolute values)\n")
	out("accounting: %s\n", loaded and "skynet.memory 可用" or "skynet.memory 未载入")

	memline("start")

	-- A/B：pack 出来的消息缓冲区就是 skynet_malloc/skynet_free 的一次往返。
	local big = string.rep("x", 4096)
	bench("pack+trash  4 KB", 100000, 4096, function()
		for _ = 1, 100000 do
			local msg, sz = skynet.pack(big)
			skynet.trash(msg, sz)
		end
	end)

	local small = string.rep("y", 64)
	bench("pack+trash  64 B", 400000, 64, function()
		for _ = 400000 * 0 + 1, 400000 do
			local msg, sz = skynet.pack(small)
			skynet.trash(msg, sz)
		end
	end)

	-- C：VM 侧的分配走 skynet_lalloc（realloc/free），与上面两条不同的一条路。
	bench("lua table churn", 200000, 0, function()
		for _ = 1, 200000 do
			local t = {}
			for j = 1, 32 do t[j] = j end
		end
	end)

	-- D：rlcore（redislib 内核）里的 sds/整数集合分配。没编 redislib 就跳过。
	local ok_hll, hll = pcall(require, "hll")
	if ok_hll then
		bench("hll add (rlcore)", 200000, 0, function()
			local h = hll.new()
			for i = 1, 200000 do
				h:pfadd("member-" .. i)
			end
		end)
	else
		out("%-26s skipped (redislib not built)\n", "hll add (rlcore)")
	end

	memline("end")
	out("alloc-bench done\n")
	os.exit(0)
end)
