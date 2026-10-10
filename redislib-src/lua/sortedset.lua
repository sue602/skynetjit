-- sortedset.lua -- business-facing module for the redislib sorted set (Redis
-- zset value + the Z* command set of Redis 8.10.0). The C module lives in the
-- redis/ sub namespace; aliases and table-form argument normalization are
-- layered on top in Task 7.
return require "redis.sortedset"
