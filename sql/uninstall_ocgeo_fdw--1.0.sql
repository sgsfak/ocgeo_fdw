SET client_min_messages = warning;

BEGIN;

-- You can use this statements as
-- template for your extension.
DROP EXTENSION ocgeo_fdw CASCADE;
DROP FUNCTION ocgeo_fdw_handler();
DROP FUNCTION ocgeo_fdw_validator(text[], oid);
DROP FUNCTION ocgeo_stats(name DEFAULT NULL);
COMMIT;