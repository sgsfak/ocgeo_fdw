#ifndef OCGEO_API__H
#define OCGEO_API__H

#include <curl/curl.h>
#include <stdbool.h>

/* HTTP Status code used */
#define OCGEO_CODE_OK (200)
#define OCGEO_CODE_INV_REQUEST (400)	/*Invalid request (bad request; a required parameter is missing; invalid coordinates; invalid version; invalid format) */
#define OCGEO_CODE_AUTH_ERROR (401) 	/* Unable to authenticate - missing, invalid, or unknown API key */
#define OCGEO_CODE_QUOTA_ERROR (402) 	/* Valid request but quota exceeded (payment required) */
#define OCGEO_CODE_FORBIDDEN (403) 		/* Forbidden (API key blocked) */
#define OCGEO_CODE_INV_ENDPOINT (404) 	/* Invalid API endpoint */
#define OCGEO_CODE_INV_METHOD (405) 	/* Method not allowed (non-GET request) */
#define OCGEO_CODE_TIMEOUT (408)  		/* Timeout; you can try again */
#define OCGEO_CODE_LONG_REQUEST (410) 	/* Request too long */
#define OCGEO_CODE_MANY_REQUESTS (429)	/* Too many requests (too quickly, rate limiting) */
#define OCGEO_CODE_INTERNAL_ERROR (503)	/* Internal server error  */

struct cJSON;
typedef struct ocgeo_status {
	int code;
	char* message;
} ocgeo_status_t;

typedef struct ocgeo_rate_info {
    int limit;
    int remaining;
    int reset;
} ocgeo_rate_info_t;

/*
 * A point using the geographic coordinate reference system, using the World
 * Geodetic System 1984 (WGS 84), with longitude and latitude units of decimal
 * degrees
 */
typedef struct ocgeo_latlng {
       double lat;
       double lng;
} ocgeo_latlng_t;

/*
 * A "bounding box" providing the SouthWest (min longitude, min latitude)
 * and NorthEast (max longitude, max latitude) points
 */
typedef struct ocgeo_latlng_bounds {
       ocgeo_latlng_t northeast;
       ocgeo_latlng_t southwest;
} ocgeo_latlng_bounds_t;

typedef struct ocgeo_result {

	/* "bounds", may be "null" (invalid): */
	ocgeo_latlng_bounds_t* bounds;

	/* "geomentry" info */
	ocgeo_latlng_t geometry;

	int confidence;

	struct ocgeo_result* next;
	struct cJSON* internal;
} ocgeo_result_t;

typedef struct ocgeo_response {
	/* Returned status */
	ocgeo_status_t status;
    char* url; /* the actual URL used, based on the given params */

	/* Rate information. If not returned (e.g. for paying customers)
	   all its fields should be 0.
	 */
	ocgeo_rate_info_t rateInfo;

	int total_results;
	ocgeo_result_t* results;
} ocgeo_response_t;

typedef struct ocgeo_params {
	/*
	 * Normal parameters : 
	 */
	
	/* Used only for forward geocoding. Restricts results to the specified 
	   country/territory or countries. The country code is a two letter code as 
	   defined by the ISO 3166-1 Alpha 2 standard. E.g. gb for the
	   United Kingdom, fr for France, us for United States. */
	char* countrycode;

	/* An IETF format language code (such as es for Spanish or pt-BR for 
	   Brazilian Portuguese), or native in which case we will attempt to return
	   the response in the local language(s). */
	char* language;

	/* The maximum number of results we should return. Default is 10. 
	   Maximum allowable value is 100.*/
	int limit;

	/* An integer from 1-10. Only results with at least this confidence will be returned. */
	int min_confidence;
	
	/* When true results will not contain annotations. */
	bool no_annotations;
	
	/* When true results will not be deduplicated. */
	bool no_dedupe;
	
	/* When true the query contents are not logged. Please use if you have 
	   concerns about privacy and want us to have no record of your query.*/
	bool no_record;

	/* Used only for forward geocoding. Provides the geocoder with a hint to 
	   bias results in favour of those closer to the specified location. Please 
	   note though, this is just one of many factors in the internal scoring we 
	   use for ranking results. */
	ocgeo_latlng_t proximity;

	/* When true the behaviour of the geocoder is changed to 
	   attempt to match the nearest road (as opposed to address). */
	bool roadinfo;
} ocgeo_params_t;

/*
 * The C API :
 */

struct ocgeo_api;

struct ocgeo_api*
ocgeo_init(const char* api_key, const char* server);

void
ocgeo_close(struct ocgeo_api* api);

/* Create a parameters "object" with default values.
 * The default values for all the fields are 0 or NULL (for the pointer fields)
 * and "invalid" latitude/longitude values for the ones (such as `proximity`)
 * that correspond to coordinates.
 */
ocgeo_params_t ocgeo_default_params(void);

/* Make a forward request i.e. find information about an address, place etc.
   Returns false if there was an error contacting the server or parsing the
   returned JSON reply.
   You can supply NULL as `params` and the default values will be used
*/
bool ocgeo_forward(struct ocgeo_api* api, const char* query, ocgeo_params_t* params, ocgeo_response_t* response);
 
/* Make a reverse request i.e. find what exists in the given latitude and longtitude.
   Returns false if there was an error contacting the server or parsing the
   returned JSON reply.
   You can supply NULL as `params` and the default values will be used
*/
bool ocgeo_reverse(struct ocgeo_api* api, double lat, double lng, ocgeo_params_t* params, ocgeo_response_t* response);

/* Free the memory used by the response of a forward or reverse call */
void ocgeo_response_cleanup(struct ocgeo_api* api, ocgeo_response_t* r);

static inline
bool ocgeo_response_ok(ocgeo_response_t* response)
{
    if (response == NULL)
        return false;
    return response->status.code == OCGEO_CODE_OK;
}

/*
 * JSON traversing API!
 * This is useful for accessing the fields of the returned JSON document
 * in a generic way, since many of the fields may be missing or new fields
 * may be added in the future.
 *
 * The caller provides a "path" string that contains a series of fields
 * separated by dots ('.') to access any internal field value. Positive
 * integer path segments are interpreted as indices in JSON arrays.
 *
 * Examples of paths:
 *  - "annotations.DMS.lat": get the "lat" value, in the "DMS" field of
 *               the "annotation" field in response
 *  - "annotations.currency.alternate_symbols.1": get the value at index 1 (2nd elem)
 *                in the "alternate_symbols" field (which is a JSON array) of the
 *                "currency" annotation
 */
/* Return as string value. The caller should not alter the string, or free the returned
   pointer, as it points to internally managed memory. If successful (i.e. the field
   exists and has no 'null' value), `ok` will set to true */
const char* ocgeo_response_get_str(ocgeo_result_t* r, const char* path, bool* ok);
/* Return as int value. If successful (i.e. the field exists and has no 'null' value),
  `ok` will set to true */
int ocgeo_response_get_int(ocgeo_result_t* r, const char* path, bool* ok);
/* Return as double value. If successful (i.e. the field exists and has no 'null' value),
  `ok` will set to true */
double ocgeo_response_get_dbl(ocgeo_result_t* r, const char* path, bool* ok);
#endif

