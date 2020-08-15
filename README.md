# Foreign Data Wrapper for OpenCageData API

This PostgreSQL extension implements a Foreign Data Wrapper (FDW) for the
[OpenCageData API](https://opencagedata.com/api). 

*Please note that this version of ocgeo_fdw works with PostgreSQL version 9.6, 10, 11, and 12.*

## Installation

To build on POSIX-compliant systems you need to ensure that the PostgreSQL "-dev" package in
installed and that the `pg_config` executable is in your path when you run make. This executable
is typically in your PostgreSQL installation's bin directory. E.g. in a recent 
[Ubuntu Linux](https://www.postgresql.org/download/linux/ubuntu/) you can install 
`postgresql-server-dev-12` :

```bash
sudo apt-get install postgresql-server-dev-12
```

Then it's a matter of running (GNU) make like so:

```bash
make && sudo make install
```

## Usage

### Load the extension in your database

```sql
CREATE EXTENSION ocgeo_fdw;
```

### Create a "Server"

The server provides global information about the OpenCageData API. The only configuration 
option required is the API endpoint. The server can be defined as follows:

```sql
CREATE SERVER ocdata_server FOREIGN DATA WRAPPER ocgeo_fdw 
        OPTIONS (uri 'https://api.opencagedata.com/geocode/v1/json');
```
The name of option for the endpoint should be `uri` and the URL given should correspond to 
the API returning [JSON format](https://opencagedata.com/api#request). Other than that, the name 
of the server can be anything you like, instead of `ocdata_server`.

### Create a "user mapping"
Different users of your database can have different keys for accessing the API. The following
statement is necessary for configuring the API key used for the current user.

```sql
CREATE USER MAPPING FOR current_user SERVER ocdata_server 
       OPTIONS (api_key '6d0e711d72d74daeb2b0bfd2a5cdfdba');
```
*Please make sure that you are using the correct server name you have defined if you specified
something different in the `CREATE SERVER` command*

For testing purposes the above SQL command will store the `6d0e711d72d74daeb2b0bfd2a5cdfdba`
[testing API key](https://opencagedata.com/api#testingkeys) that returns successful, but always
the same, responses. The above can be used to check whether the FDW works properly but you can
register for the [free plan](https://opencagedata.com/pricing) to get a valid key for more 
extensive testing purposes, or, better yet, become a customer!. 

If you want to change the API key, you can use the following:

```sql
ALTER USER MAPPING FOR current_user SERVER ocdata_server
      OPTIONS (SET api_key '<your API key>');
```

### Create a "foreign table"
The foreign table is a "virtual" table allowing to make requests to the OpenCageData API when used
in SQL SELECT commands. The following is a typical definition that allows to all the informtion 
returned by the JSON API in the [`components` and `formatted` fields](https://opencagedata.com/api#formatted):

```sql
CREATE FOREIGN TABLE ocgdata_api (
        json_response JSONB,
        bounding_box BOX,
        location POINT,
        _category TEXT,
        _type TEXT,
        city TEXT,
        city_district TEXT,
        continent TEXT,
        country TEXT,
        country_code TEXT,
        county TEXT,
        house_number TEXT,
        political_union TEXT,
        neighbourhood TEXT,
        postcode TEXT,
        road TEXT,
        road_type TEXT,
        state TEXT,
        state_code TEXT,
        state_district TEXT,
        suburb TEXT,

        confidence INTEGER,
        formatted TEXT,

        -- The following is the input to the select 
        q text
) SERVER ocdata_server;
```

The name of the table can be anything you like (instead of `ocgdata_api`) but, as 
with the definition of the User Mapping, it is important to use the correct server name.

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

## More advanced queries

### How many cities named Paris exist all over the world?

```sql
> SELECT country, COUNT(*) FROM ocgdata_api WHERE q='paris' AND _type='city' GROUP BY country ORDER BY 2;
+--------------------------+-------+
|         country          | count |
+--------------------------+-------+
| France                   |     1 |
| Canada                   |     2 |
| United States of America |     7 |
+--------------------------+-------+
```

### Joining with other tables

Assuming we have a (local) table with our users and there's an `address` attribute with
the user supplied home address, we can join the two tables to get geocoding information
as follows:

```sql
SELECT user_id, ocgdata_api.*
FROM users LEFT JOIN  ocgdata_api ON q=users.address
WHERE confidence >= 5;
```

For executing this, PostgreSQL will select a "nested loop" plan where for each row in the 
`users` table an API call will be performed through the FDW. So this will result in making 
as many API requests as rows in the `users` table. 

**Note: This may result in a large number of API requests sent in quick succession. The FDW
currently does not perform any throttling according to the user's plan, so make sure that your
current plan allows this. To check this, you need to have an estimate of the request latency
and of course the current limits on your plan. For the latter, see the `ocgeo_stats` function
described bellow.**

If such command is used frequently, it may be a good idea to create a [`materialized` view](https://www.postgresql.org/docs/current/static/rules-materializedviews.html):

```sql
CREATE MATERIALIZED VIEW users_locations AS 
SELECT user_id, ocgdata_api.*
FROM users LEFT JOIN ocgdata_api ON q=users.address
```

..and then perform any subsequent queries on the local materialized view. When the `users`
base table is updated you need to perform a `REFRESH MATERIALIZED VIEW users_locations` to
recreate the contents of the view.

### Accessing the JSON results

If the definition of the foreign table includes an attribute of type [JSONB](https://www.postgresql.org/docs/12/datatype-json.html), `ocgeo_fdw` will store the JSON result message there. This permits more information to be retrieved since there  are plenty PostgreSQL [operators and functions](https://www.postgresql.org/docs/12/functions-json.html) for processing JSON data. For example, we can get the `currency` information from the [`annotations` field](https://opencagedata.com/api#annotations) of the JSON result, as shown next:

```sql
WITH temp(js) AS 
   (SELECT json_response FROM ocgdata_api WHERE q='taj mahal, India') 
SELECT jsonb_pretty(js->'annotations'->'currency') AS currency
FROM temp;
```
```
+---------------------------------+
│            currency             │
+---------------------------------+
│ {                               │
│     "name": "Indian Rupee",     │
│     "symbol": "₹",              │
│     "subunit": "Paisa",         │
│     "iso_code": "INR",          │
│     "html_entity": "&#x20b9;",  │
│     "iso_numeric": "356",       │
│     "decimal_mark": ".",        │
│     "symbol_first": 1,          │
│     "subunit_to_unit": 100,     │
│     "alternate_symbols": [      │
│         "Rs",                   │
│         "৳",                    │
│         "૱",                   │
│         "௹",                    │
│         "रु",                    │
│         "₨"                     │
│     ],                          │
│     "thousands_separator": ",", │
│     "smallest_denomination": 50 │
│ }                               │
+---------------------------------+
```

Here I am using a "Common Table Expression" (CTE) ([`WITH` query](https://www.postgresql.org/docs/12/queries-with.html)) to define the subquery `temp` to the foreign table which is then used in the main query.

## Miscellaneous

### Debug messages

Using the following in the `psql` command session you can debug information and the actual API
URL used:

```sql
SET client_min_messages TO DEBUG1;
```

For example, the following:
```sql
SELECT confidence, _type, _category, location, formatted 
FROM ocgdata_api 
WHERE q='Trierer Straße 15, 99423, Weimar, Deutschland';
```
will print:
```
DEBUG:  ocgeoGetForeignRelSize: remote conds: 1, local conds: 0
DEBUG:  ocgeoGetForeignPaths: param paths 0
DEBUG:  printRestrictInfoList: (q = 'Trierer Straße 15, 99423, Weimar, Deutschland')
DEBUG:  ocgeoGetForeignPlan, 6 column list, 1 scan clauses
DEBUG:  function ocgeoBeginForeignScan qual: q='Trierer Straße 15, 99423, Weimar, Deutschland' and confidence>=0
DEBUG:  API https://api.opencagedata.com/geocode/v1/json?q=Trierer%20Stra%C3%9Fe%2015%2C%2099423%2C%20Weimar%2C%20Deutschland&key=6d0e711d72d74daeb2b0bfd2a5cdfdba&limit=50&no_annotations=0 returned status: 200, results: 1, time: 1226.36 msec
```

### Statistics (usage) information

This FDW provides an `ocgeo_stats` function that can be used to retrieve information about the
usage of the API in the current session. The information returned contains:

* The total number of API calls (requests)
* The total number of failed API calls (e.g. because of network errors, rate limiting, etc)
* The total number of seconds taken in the submission of the API requests and the parsing of
  the JSON response, for *all* calls (successful and failed)
* The [rate limit information](https://opencagedata.com/api#rate-limiting) as returned by the
  API in the most recent call. This includes the limit (i.e. the maximum number of requests per
  day), the remaining API calls in the current day, and the date time when the counter will be
  reset.

You can use this function as follows:

```sql
> SELECT * FROM ocgeo_stats();
+-----------+------------+------------+------------+----------------+------------------------+
| nbr_calls | nbr_failed | total_time | rate_limit | rate_remaining |       rate_reset       |
+-----------+------------+------------+------------+----------------+------------------------+
|         4 |          0 |       3.09 |       2500 |           2482 | 2020-07-15 03:00:00+03 |
+-----------+------------+------------+------------+----------------+------------------------+
```

The rate information is returned by the API server on each request so it is the most accurate,
according to the most recent API call. On the other hand, the number of calls and total time 
per session so they are "local" to the current PostgreSQL connection. So for example based on the
above we see that we have 2482 remaining API calls in the current "day", and therefore we have
made 2500 - 2482 = 8 requests, but only 4 of them were performed in the current session.
