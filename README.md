Foreign Data Wrapper for OpenCageData API
=========================================


```
DROP EXTENSION IF EXISTS ocgeo_fdw CASCADE;

CREATE EXTENSION ocgeo_fdw;
CREATE SERVER ocdata_server FOREIGN DATA WRAPPER ocgeo_fdw 
	OPTIONS (uri 'https://api.opencagedata.com/geocode/v1/json');
CREATE USER MAPPING FOR current_user SERVER ocdata_server 
	OPTIONS (api_key '6d0e711d72d74daeb2b0bfd2a5cdfdba');
CREATE FOREIGN TABLE ocgdata_api (
    reply text,

    query text
) SERVER ocdata_server;
```