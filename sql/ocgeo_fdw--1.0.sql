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

-- CREATE FUNCTION ocgeo_stats(name DEFAULT NULL) RETURNS text
-- AS 'MODULE_PATHNAME'
-- LANGUAGE C STABLE CALLED ON NULL INPUT;

-- COMMENT ON FUNCTION ocgeo_stats(name)
-- IS 'shows the information, version of ocgeo_fdw, PostgreSQL, and stats of its usage';

CREATE FOREIGN DATA WRAPPER ocgeo_fdw
  HANDLER ocgeo_fdw_handler
  VALIDATOR ocgeo_fdw_validator;

COMMENT ON FOREIGN DATA WRAPPER ocgeo_fdw
IS 'OpenCageData API foreign data wrapper';