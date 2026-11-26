#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/capture_metadata.c"
#include "../src/archive_naming.c"
#include "../src/transfer_queue.c"

static const char *prefix="ESP-AABBCCDDEEFF";
int main(void) {
    transfer_job_t job={0},parsed;
    strcpy(job.prefix,prefix); memset(job.capture_id,'c',64); memset(job.request_id,'d',64); memset(job.sha256,'a',64);
    job.sequence=42; job.bytes=123456;
    capture_metadata_t capture={"2026-11-26 14:30:05","test"};
    assert(archive_media_path(job.path,sizeof(job.path),"/sd/stonelink",prefix,"anywhere.mp4",&capture,job.sha256,job.capture_id,"HD"));
    char sd_path[768]; assert(archive_quality_path(sd_path,sizeof(sd_path),job.path,prefix,"SD"));
    strcpy(job.requested_filename,strrchr(sd_path,'/')+1);
    char json[4096]; assert(transfer_job_json(json,sizeof(json),&job));
    assert(transfer_job_parse(json,&parsed,prefix,job.request_id));
    assert(parsed.bytes==job.bytes && parsed.sequence==42 && !strcmp(parsed.path,job.path));
    assert(!strcmp(parsed.requested_filename,job.requested_filename));
    assert(!transfer_job_json(json,10,&job));
    assert(transfer_job_json(json,sizeof(json),&job));
    assert(!transfer_job_parse(json,&parsed,"ESP-FFFFFFFFFFFF",job.request_id));
    char wrong_id[65]; memset(wrong_id,'e',64); wrong_id[64]=0;
    assert(!transfer_job_parse(json,&parsed,prefix,wrong_id));
    strcat(json,"garbage"); assert(!transfer_job_parse(json,&parsed,prefix,job.request_id));
    job.sequence=0; assert(transfer_job_json(json,sizeof(json),&job)); assert(!transfer_job_parse(json,&parsed,prefix,job.request_id));
    job.sequence=42;
    strcpy(job.path,sd_path); assert(transfer_job_json(json,sizeof(json),&job));
    assert(!transfer_job_parse(json,&parsed,prefix,job.request_id)); // Queue only real HD flags.
    strcpy(job.path,"/usb/camera.mp4"); assert(transfer_job_json(json,sizeof(json),&job)); assert(!transfer_job_parse(json,&parsed,prefix,job.request_id));
    strcpy(job.path,"/sd/stonelink/../other.mp4"); assert(transfer_job_json(json,sizeof(json),&job)); assert(!transfer_job_parse(json,&parsed,prefix,job.request_id));
    assert(archive_media_path(job.path,sizeof(job.path),"/sd/stonelink",prefix,"anywhere.mp4",&capture,job.sha256,job.capture_id,"HD"));
    job.sha256[0]='f'; assert(transfer_job_json(json,sizeof(json),&job)); assert(!transfer_job_parse(json,&parsed,prefix,job.request_id));
    job.sha256[0]='a';
    assert(transfer_job_json(json,sizeof(json),&job));
    cJSON *document=cJSON_Parse(json); assert(document);
    cJSON_SetNumberValue(cJSON_GetObjectItem(document,"sequence"),1.5);
    assert(cJSON_PrintPreallocated(document,json,sizeof(json),false));
    assert(!transfer_job_parse(json,&parsed,prefix,job.request_id));
    cJSON_Delete(document);
    puts("PASS HD request JSON: persistence roundtrip, exact filename/SHA, device/group/path guards, malformed input");
    return 0;
}
