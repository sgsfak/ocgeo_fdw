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

CREATE OR REPLACE FUNCTION ocgeo_stats(OUT nbr_calls bigint, OUT nbr_failed bigint, OUT total_time float8)
    RETURNS SETOF record
    AS 'MODULE_PATHNAME', 'ocgeo_stats'
    LANGUAGE C IMMUTABLE STRICT;

COMMENT ON FUNCTION ocgeo_stats()
 IS 'shows the stats of usage: number of API calls, number of failed ones, total nbr of seconds';

CREATE FOREIGN DATA WRAPPER ocgeo_fdw
  HANDLER ocgeo_fdw_handler
  VALIDATOR ocgeo_fdw_validator;

COMMENT ON FOREIGN DATA WRAPPER ocgeo_fdw
IS 'OpenCageData API foreign data wrapper';

