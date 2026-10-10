-- hll.lua -- business-facing module for the redislib HyperLogLog (Redis HLL
-- string value + PFADD/PFCOUNT/PFMERGE/PFDEBUG/PFSELFTEST). The C module lives
-- in the redis/ sub namespace; aliases and table-form argument normalization
-- are layered on top in Task 7.
return require "redis.hll"
