CREATE FUNCTION ocgeo_fdw_handler() RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

COMMENT ON FUNCTION ocgeo_fdw_handler()
IS 'OpenCageData API foreign data wrapper handler';

CREATE FUNCTION ocgeo_fdw_validator(text[], oid) RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

COMMENT ON FUNCTION ocgeo_fdw_validator(text[], oid)
IS 'OpenCageData API foreign data wrapper options validator';

CREATE OR REPLACE FUNCTION ocgeo_stats(
	OUT nbr_calls integer, OUT nbr_failed integer, OUT total_time float8,
	OUT rate_limit integer, OUT rate_remaining integer, OUT rate_reset timestamptz)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'ocgeo_stats'
LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION ocgeo_stats()
 IS 'shows the stats of usage: nbr of API calls, nbr of failed ones, total nbr of seconds, and rate info';

CREATE FOREIGN DATA WRAPPER ocgeo_fdw
  HANDLER ocgeo_fdw_handler
  VALIDATOR ocgeo_fdw_validator;

COMMENT ON FOREIGN DATA WRAPPER ocgeo_fdw
IS 'OpenCageData API foreign data wrapper';

