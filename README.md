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
        json_response jsonb,
        bounding_box box,
        location point,
        _category text,
        _type text,
        city text,
        city_district text,
        continent text,
        country text,
        country_code text,
        county text,
        house_number text,
        political_union text,
        neighbourhood text,
        postcode text,
        road text,
        road_type text,
        state text,
        state_code text,
        state_district text,
        suburb text,

        confidence int,
        formatted text,

        -- The following is the input to the select 
        q text
) SERVER ocdata_server;
```
