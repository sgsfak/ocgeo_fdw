/*
  Copyright (c) 2019 Stelios Sfakianakis
  
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
  
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "cJSON.h"
#include "sds.h"
#include "ocgeo_api.h"

#ifndef OCG_API_SERVER
#define OCG_API_SERVER "https://api.opencagedata.com/geocode/v1/json"
#endif

#ifndef OCGEO_VERSION
#define OCGEO_VERSION "0.3.1"
#endif


#ifdef NDEBUG
#define log(...)
#else
#define log(...) fprintf (stderr, __VA_ARGS__)
#endif

char* ocgeo_version = OCGEO_VERSION;



struct ocgeo_api {
    CURL* curl;
    // void *(*malloc_fn)(size_t sz);
    // void (*free_fn)(void *ptr);
    cJSON_Hooks memfns;
    char* api_key;
    char* server;
};

struct ocgeo_api*
ocgeo_init(const char* api_key, const char* server, 
    void *(*malloc_fn)(size_t sz), void (*free_fn)(void *ptr))
{
    elog(DEBUG1,"entering function %s, ocgeo init",__func__);
    struct ocgeo_api* api = (*malloc_fn) (sizeof(*api));
    elog(DEBUG1,"in function %s, before curl init",__func__);
    api->curl = curl_easy_init();
    elog(DEBUG1,"in function %s, after curl init",__func__);
    
    api->memfns.malloc_fn = malloc_fn;
    api->memfns.free_fn = free_fn;
    api->api_key = (*malloc_fn)(strlen(api_key) + 1);
    strcpy(api->api_key, api_key);
    api->server = (*malloc_fn)(strlen(server) + 1);
    strcpy(api->server, server);
    
    cJSON_InitHooks(&api->memfns);

    return api;
}

void
ocgeo_close(struct ocgeo_api* api)
{
    if (api == NULL)
        return;
    (*api->memfns.free_fn)(api->server);
    (*api->memfns.free_fn)(api->api_key);
    (*api->memfns.free_fn)(api);
}


static ocgeo_latlng_t ocgeo_invalid_point = {.lat = -91.0, .lng=-181};

struct http_response {
    sds data;
};

static size_t
write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct http_response* r = userdata;
    r->data = sdscatlen(r->data, ptr, size*nmemb);
    return nmemb;
}

#define JSON_INT_VALUE(json) ((json) == NULL || cJSON_IsNull(json) ? 0 : (json)->valueint)
#define JSON_OBJ_GET_STR(obj,name) (cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj,name)))
#define JSON_OBJ_GET_INT(obj,name) (cJSON_GetObjectItemCaseSensitive(obj,name)->valueint)

static inline void
parse_latlng(cJSON* json, ocgeo_latlng_t* latlng)
{
    latlng->lat = cJSON_GetObjectItemCaseSensitive(json,"lat")->valuedouble;
    latlng->lng = cJSON_GetObjectItemCaseSensitive(json,"lng")->valuedouble;
}

static int
parse_response_json(cJSON* json, ocgeo_response_t* response)
{
    cJSON* obj = NULL;

    memset(response, 0, sizeof(ocgeo_response_t));
    response->results = NULL;
    response->internal = json;

    obj = cJSON_GetObjectItemCaseSensitive(json, "status");
    assert(obj);
    response->status.code = JSON_OBJ_GET_INT(obj,"code");
    response->status.message = JSON_OBJ_GET_STR(obj,"message");

    /* Rate information, may not returned (e.g. for paying customers): */
    obj = cJSON_GetObjectItem(json, "rate");
    if (obj) {
        response->rateInfo.limit = JSON_OBJ_GET_INT(obj,"limit");
        response->rateInfo.remaining = JSON_OBJ_GET_INT(obj,"remaining");
        response->rateInfo.reset = JSON_OBJ_GET_INT(obj,"reset");
    }

    obj = cJSON_GetObjectItem(json, "total_results");
    assert(obj);
    response->total_results = JSON_INT_VALUE(obj);
    if (response->total_results <= 0) {
        return 0;
    }

    response->results = malloc(response->total_results * sizeof(ocgeo_result_t));
    obj = cJSON_GetObjectItemCaseSensitive(json, "results");
    assert(obj);

    cJSON* result_js;
    int k = 0;
    ocgeo_result_t proto = {0};
    ocgeo_result_t** pprev = &response->results;
    for (result_js = obj->child; result_js!= NULL; result_js = result_js->next, k++) {
        ocgeo_result_t* result = response->results + k;
        *result = proto; /* initialize with 0/NULL values */
        result->internal = result_js;
        /* keep them in a list for easy traversal */
        (*pprev) = result;
        pprev = &result->next;

        result->confidence = JSON_OBJ_GET_INT(result_js,"confidence");

        cJSON* bounds_js = cJSON_GetObjectItemCaseSensitive(result_js, "bounds");
        // assert(bounds_js);
        if (bounds_js) {
            result->bounds = calloc(1, sizeof(ocgeo_latlng_bounds_t));
            parse_latlng(cJSON_GetObjectItem(bounds_js, "northeast"), &result->bounds->northeast);
            parse_latlng(cJSON_GetObjectItem(bounds_js, "southwest"), &result->bounds->southwest);
        }

        cJSON* geom_js = cJSON_GetObjectItemCaseSensitive(result_js, "geometry");
        // assert(geom_js);
        if (geom_js)
            parse_latlng(geom_js, &result->geometry);
        else
            result->geometry = ocgeo_invalid_point;
        

        cJSON* comp_js = cJSON_GetObjectItemCaseSensitive(result_js, "components");
        assert(comp_js);
        result->ISO_alpha2 = JSON_OBJ_GET_STR(comp_js, "ISO_3166-1_alpha-2");
        result->ISO_alpha3 = JSON_OBJ_GET_STR(comp_js, "ISO_3166-1_alpha-3");
        result->type = JSON_OBJ_GET_STR(comp_js, "_type");
        result->category = JSON_OBJ_GET_STR(comp_js, "_category");
        result->city = JSON_OBJ_GET_STR(comp_js, "city");
        result->city_district = JSON_OBJ_GET_STR(comp_js, "city_district");
        result->continent = JSON_OBJ_GET_STR(comp_js, "continent");
        result->country = JSON_OBJ_GET_STR(comp_js, "country");
        result->country_code = JSON_OBJ_GET_STR(comp_js, "country_code");
        result->county = JSON_OBJ_GET_STR(comp_js, "county");
        result->house_number = JSON_OBJ_GET_STR(comp_js, "house_number");
        result->neighbourhood = JSON_OBJ_GET_STR(comp_js, "neighbourhood");
        result->political_union = JSON_OBJ_GET_STR(comp_js, "political_union");
        result->postcode = JSON_OBJ_GET_STR(comp_js, "postcode");
        result->road = JSON_OBJ_GET_STR(comp_js, "road");
        result->state = JSON_OBJ_GET_STR(comp_js, "state");
        result->state_district = JSON_OBJ_GET_STR(comp_js, "state_district");
        result->suburb = JSON_OBJ_GET_STR(comp_js, "suburb");

        /* Parse annotations, if exist */
        cJSON* ann_js = cJSON_GetObjectItemCaseSensitive(result_js, "annotations");
        if (ann_js) {
            result->callingcode = JSON_OBJ_GET_INT(ann_js,"callingcode");
            
            cJSON* tm_ann = cJSON_GetObjectItemCaseSensitive(ann_js, "timezone");
            if (tm_ann) {
                ocgeo_ann_timezone_t* timezone = calloc(1, sizeof(ocgeo_ann_timezone_t));
                timezone->name = JSON_OBJ_GET_STR(tm_ann, "name");
                timezone->short_name = JSON_OBJ_GET_STR(tm_ann, "short_name");
                timezone->offset_string = JSON_OBJ_GET_STR(tm_ann, "offset_string");
                timezone->offset_sec = JSON_OBJ_GET_INT(tm_ann,"offset_sec");
                timezone->now_in_dst = JSON_OBJ_GET_INT(tm_ann,"now_in_dst") == 1;
                result->timezone = timezone;
            }
            cJSON* ri_ann = cJSON_GetObjectItemCaseSensitive(ann_js, "roadinfo");
            if (ri_ann) {
                ocgeo_ann_roadinfo_t* roadinfo = calloc(1, sizeof(ocgeo_ann_roadinfo_t));
                roadinfo->drive_on = JSON_OBJ_GET_STR(ri_ann, "drive_on");
                roadinfo->speed_in = JSON_OBJ_GET_STR(ri_ann, "speed_in");
                roadinfo->road = JSON_OBJ_GET_STR(ri_ann, "road");
                roadinfo->road_type = JSON_OBJ_GET_STR(ri_ann, "road_type");
                roadinfo->surface = JSON_OBJ_GET_STR(ri_ann, "surface");
                result->roadinfo = roadinfo;
            }
            cJSON* cur_ann = cJSON_GetObjectItemCaseSensitive(ann_js, "currency");
            if (cur_ann) {
                ocgeo_ann_currency_t* currency = calloc(1, sizeof(ocgeo_ann_currency_t));
                currency->name = JSON_OBJ_GET_STR(cur_ann, "name");
                currency->iso_code = JSON_OBJ_GET_STR(cur_ann, "iso_code");
                currency->symbol = JSON_OBJ_GET_STR(cur_ann, "symbol");
                currency->decimal_mark = JSON_OBJ_GET_STR(cur_ann, "decimal_mark");
                currency->thousands_separator = JSON_OBJ_GET_STR(cur_ann, "thousands_separator");
                result->currency = currency;
            }
            result->geohash = JSON_OBJ_GET_STR(ann_js, "geohash");
            cJSON* w3w_ann = cJSON_GetObjectItemCaseSensitive(ann_js, "what3words");
            if (w3w_ann) {
                result->what3words = JSON_OBJ_GET_STR(w3w_ann, "words");
            }
        }
    }
    return 0;
}

static ocgeo_response_t*
do_request(CURL* curl, bool is_fwd, const char* q, 
           const char* api_key, const char* server,
           ocgeo_params_t* params, ocgeo_response_t* response)
{
    if (params == NULL) {
        ocgeo_params_t params = ocgeo_default_params();
        return do_request(curl, is_fwd, q, api_key, server, &params, response);
    }

    /* Make sure that we have a proper response: */
    if (response == NULL)
        return NULL;

    // Build URL:
    char* q_escaped = curl_easy_escape(curl, q, 0);
    sds url = sdsempty();
    url = sdscatprintf(url, "%s?q=%s&key=%s", server, q_escaped, api_key);
    curl_free(q_escaped);
    if (is_fwd && params->countrycode)
        url = sdscatprintf(url, "&countrycode=%s", params->countrycode);
    if (params->language)
        url = sdscatprintf(url, "&language=%s", params->language);
    if (params->limit)
        url = sdscatprintf(url, "&limit=%d", params->limit);
    if (params->min_confidence)
        url = sdscatprintf(url, "&min_confidence=%d", params->min_confidence);
    url = sdscatprintf(url, "&no_annotations=%d", params->no_annotations ? 1 : 0);
    if (params->no_dedupe)
        url = sdscat(url, "&no_dedupe=1");
    if (params->no_record)
        url = sdscat(url, "&no_record=1");
    if (is_fwd && params->roadinfo)
        url = sdscat(url, "&roadinfo=1");
    if (is_fwd && ocgeo_is_valid_latlng(params->proximity))
        url = sdscatprintf(url, "&proximity=%.8F,%.8F", params->proximity.lat, params->proximity.lng);

    // log("URL=%s\n", url);

    struct http_response r; r.data = sdsempty();
    sds user_agent = sdsempty();
    user_agent = sdscatprintf(user_agent, "c-ocgeo/%s (%s)", ocgeo_version, curl_version());
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    sdsfree(user_agent);

    if (res != CURLE_OK) {
        sdsfree(r.data);
        return NULL;
    }

    cJSON* json = cJSON_Parse(r.data);
    sdsfree(r.data);

    if (json == NULL)
        return NULL;

    parse_response_json(json, response);
    return response;
}

ocgeo_params_t ocgeo_default_params()
{

    ocgeo_params_t params = {0};
    params.proximity = ocgeo_invalid_point;
    return params;
}


ocgeo_response_t*
ocgeo_forward(struct ocgeo_api* api, const char* q,
        ocgeo_params_t* params, ocgeo_response_t* response)
{
    return do_request(api->curl, true, q, api->api_key, api->server, params, response);
}

ocgeo_response_t*
ocgeo_reverse(struct ocgeo_api* api, double lat, double lng, 
        ocgeo_params_t* params, ocgeo_response_t* response)
{
    sds q = sdsempty();
    q = sdscatprintf(q, "%.8F,%.8F", lat, lng);
    ocgeo_response_t* r = do_request(api->curl, false, q, api->api_key, api->server, params, response);
    sdsfree(q);
    return r;
}


#define foreach_ocgeo_result(result,response) for(result=(response)->results;result!=NULL;result=result->next)

void ocgeo_response_cleanup(struct ocgeo_api* api, ocgeo_response_t* r)
{
    if (r == NULL)
        return;

    ocgeo_result_t* result;
    foreach_ocgeo_result(result, r) {
        api->memfns.free_fn(result->bounds);
        api->memfns.free_fn(result->timezone);
        api->memfns.free_fn(result->roadinfo);
        api->memfns.free_fn(result->currency);
    }
    r->total_results = 0;
    api->memfns.free_fn(r->results);
    r->results = NULL;
    cJSON_Delete(r->internal);
    r->internal = NULL;
}

