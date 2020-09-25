# Foreign Data Wrapper for OpenCageData API

This PostgreSQL extension implements a Foreign Data Wrapper (FDW) for the
[OpenCageData API](https://opencagedata.com/api).

## Usage

```sql
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


With the above definition in place, you can do single API calls like so:
```sql
SELECT confidence, _type, _category, location, formatted
FROM ocgdata_api
WHERE q='Trierer Straße 15, 99423, Weimar, Deutschland';
```

The important thing to remember is to use the `q` attribute to provide the search query.
Providing an "equal restriction" on `q` is required, otherwise no API call will be made
(but see below on 'More advanced queries' for JOIN queries). Giving a latitude - longitude
coordinates separated by `,` (comma) as a query will result in a
[reverse geocoding request](https://opencagedata.com/api#reverse-resp). Another attribute
that can be used as input is the `confidence`: Putting a `>=` condition on this, will make an API
request using the [`min_confidence`](https://opencagedata.com/api#confidence) parameter.

Example:
```sql
> SELECT _type, formatted FROM ocgdata_api WHERE q='eiffel tower, france' AND confidence>=5;
+-------------+-----------------------------------------------------------------------------+
│    _type    │                                  formatted                                  │
+-------------+-----------------------------------------------------------------------------+
│ attraction  │ Eiffel Tower, 5 Avenue Anatole France, 75007 Paris, France                  │
│ information │ Eiffel Tower, Esplanade des Ouvriers de la Tour Eiffel, 75007 Paris, France │
│ road        │ Rue Gustave Eiffel, 13010 Marseille, France                                 │
│ road        │ Rue Gustave Eiffel, 45000 Orléans, France                                   │
│ road        │ Rue Gustave Eiffel, 34000 Montpellier, France                               │
│ road        │ Pont Eiffel, 27000 Évreux, France                                           │
│ road        │ Rue Gustave Eiffel, 33100 Bordeaux, France                                  │
│ road        │ Rue Gustave Eiffel, 84000 Avignon, France                                   │
│ road        │ Rue Gustave Eiffel, 18000 Bourges, France                                   │
│ road        │ Avenue Gustave Eiffel, 21000 Dijon, France                                  │
│ road        │ Rue Gustave Eiffel, 72100 Le Mans, France                                   │
│ road        │ Rue Gustave Eiffel, 30000 Nîmes, France                                     │
│ road        │ Rue Gustave Eiffel, 38000 Grenoble, France                                  │
│ road        │ Impasse Eiffel, 44700 Orvault, France                                       │
│ road        │ Rue Gustave Eiffel, 82000 Montauban, France                                 │
│ road        │ Rue Gustave Eiffel, 81000 Albi, France                                      │
│ road        │ Rue Gustave Eiffel, 89000 Auxerre, France                                   │
│ road        │ Rue Gustave Eiffel, 79000 Niort, France                                     │
│ road        │ Rue Gustave Eiffel, 94000 Créteil, France                                   │
│ road        │ Rue Gustave Eiffel, 11000 Carcassonne, France                               │
│ road        │ Rue Gustave Eiffel, 22000 Saint-Brieuc, France                              │
│ road        │ Rue Gustave Eiffel, 86100 Châtellerault, France                             │
│ road        │ Rue Gustave Eiffel, 85000 La Roche-sur-Yon, France                          │
│ road        │ Rue Gustave Eiffel, 60000 Beauvais, France                                  │
│ road        │ Avenue Eiffel, 78420 Carrières-sur-Seine, France                            │
+-------------+-----------------------------------------------------------------------------+
```
