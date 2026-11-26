#include "transfer_queue.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
static const char *field(const cJSON *json,const char *name) {
    cJSON *item=cJSON_GetObjectItemCaseSensitive(json,name);
    return cJSON_IsString(item)?item->valuestring:NULL;
}
static bool number(const cJSON *json,const char *name,uint64_t maximum,uint64_t *out) {
    cJSON *item=cJSON_GetObjectItemCaseSensitive(json,name);
    if(!cJSON_IsNumber(item) || item->valuedouble<0 || item->valuedouble>(double)maximum) return false;
    uint64_t value=(uint64_t)item->valuedouble;
    if((double)value!=item->valuedouble) return false;
    *out=value; return true;
}
bool transfer_job_json(char *out,size_t capacity,const transfer_job_t *job) {
    cJSON *json=cJSON_CreateObject(); if(!json) return false;
    bool ok=cJSON_AddNumberToObject(json,"schema",1) &&
        cJSON_AddStringToObject(json,"requested_quality","HD") &&
        cJSON_AddStringToObject(json,"device_prefix",job->prefix) &&
        cJSON_AddStringToObject(json,"capture_id",job->capture_id) &&
        cJSON_AddStringToObject(json,"request_id",job->request_id) &&
        cJSON_AddStringToObject(json,"sha256",job->sha256) &&
        cJSON_AddStringToObject(json,"archive_path",job->path) &&
        cJSON_AddStringToObject(json,"requested_filename",job->requested_filename) &&
        cJSON_AddNumberToObject(json,"bytes",(double)job->bytes) &&
        cJSON_AddNumberToObject(json,"sequence",job->sequence);
    if(ok) ok=capacity<=INT32_MAX && cJSON_PrintPreallocated(json,out,(int)capacity,false);
    cJSON_Delete(json); return ok;
}
bool transfer_job_parse(const char *text,transfer_job_t *job,const char *prefix,const char *request_id) {
    memset(job,0,sizeof(*job));
    if(strlen(prefix)!=16 || !archive_hex_id(request_id)) return false;
    cJSON *json=cJSON_ParseWithOpts(text,NULL,true); if(!json) return false;
    const char *p=field(json,"device_prefix"),*id=field(json,"capture_id"),*request=field(json,"request_id"),
        *sha=field(json,"sha256"),*path=field(json,"archive_path"),*filename=field(json,"requested_filename"),*quality=field(json,"requested_quality");
    uint64_t schema,sequence,bytes;
    bool ok=number(json,"schema",1,&schema) && schema==1 && p && !strcmp(p,prefix) &&
        archive_hex_id(id) && request && !strcmp(request,request_id) && archive_hex_id(sha) &&
        quality && !strcmp(quality,"HD") && filename && strlen(filename)<sizeof(job->requested_filename) &&
        path && strlen(path)<sizeof(job->path) && !strstr(path,"..") && !strchr(path,'\\') && !strchr(path,':') &&
        number(json,"sequence",UINT32_MAX,&sequence) && sequence>0 &&
        number(json,"bytes",UINT64_C(9007199254740991),&bytes);
    if(ok) {
        char allowed[160]; archive_asset_t asset;
        snprintf(allowed,sizeof(allowed),"/sd/stonelink/%s/captures/%s/",prefix,id);
        size_t n=strlen(allowed);
        ok=!strncmp(path,allowed,n) && !strchr(path+n,'/') && archive_parse_filename(path+n,prefix,&asset) &&
            asset.capture.datetime[0] && !strcmp(asset.quality,"HD") && !strcmp(asset.sha256,sha);
    }
    if(ok) {
        strcpy(job->prefix,p); strcpy(job->capture_id,id); strcpy(job->request_id,request); strcpy(job->sha256,sha);
        strcpy(job->path,path); strcpy(job->requested_filename,filename); job->sequence=sequence; job->bytes=bytes;
    }
    cJSON_Delete(json); return ok;
}
