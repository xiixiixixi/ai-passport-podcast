#pragma once

/* Only the JSON data-tree boundary is substituted. The production controller
 * consumes fixture trees through its own field validation; parsing is not
 * under test here. No second controller or navigation model is implemented. */
typedef struct cJSON {
    struct cJSON *next, *child;
    char *string, *valuestring;
    int type, valueint;
    double valuedouble;
} cJSON;
enum { JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER, JSON_FALSE, JSON_TRUE };
cJSON *cJSON_Parse(const char *);
void cJSON_Delete(cJSON *);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *, const char *);
int cJSON_IsString(const cJSON *);
int cJSON_IsNumber(const cJSON *);
int cJSON_IsArray(const cJSON *);
int cJSON_IsTrue(const cJSON *);
int cJSON_GetArraySize(const cJSON *);
#define cJSON_ArrayForEach(element, array) \
    for ((element) = (array) ? (array)->child : 0; (element); (element) = (element)->next)
