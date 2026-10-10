-- geo.lua -- business-facing module for the redislib geoset (a Redis zset whose
-- scores are 52-bit geohashes; GEOADD/GEODIST/GEOHASH/GEOPOS/GEORADIUS[_RO]/
-- GEORADIUSBYMEMBER[_RO]/GEOSEARCH/GEOSEARCHSTORE). The C module lives in the
-- redis/ sub namespace; aliases and table-form argument normalization are
-- layered on top in Task 7.
return require "redis.geo"
