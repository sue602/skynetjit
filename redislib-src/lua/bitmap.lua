-- bitmap.lua -- business-facing module for the redislib bitmap (Redis string
-- value + SETBIT/GETBIT/BITCOUNT/BITPOS/BITOP/BITFIELD). The C module lives in
-- the redis/ sub namespace; aliases and table-form argument normalization are
-- layered on top in Task 7.
return require "redis.bitmap"
