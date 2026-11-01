#include "briqon_core.h"
#include "contract.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); return 1; } } while (0)
static const char *valid_on =
    "{\"schema_version\":1,\"device_id\":\"briqon-demo-001\","
    "\"target_boot_id\":\"boot-a\",\"target_session_id\":\"session-b\","
    "\"command_id\":\"cmd-1\",\"expires_at_uptime_ms\":15000,"
    "\"action\":\"set_output\",\"params\":{\"channel\":\"relay-1\",\"state\":\"ON\"}}";

static const char *parse_modified(const char *key, cJSON *value, bq_command_t *cmd) {
    cJSON *j=cJSON_Parse(valid_on);
    cJSON_ReplaceItemInObjectCaseSensitive(j,key,value);
    char *s=cJSON_PrintUnformatted(j);
    const char *error=bq_parse_command(s,strlen(s),cmd);
    cJSON_free(s); cJSON_Delete(j);
    return error;
}
int main(void) {
    bq_config_t c=bq_default_config(), next;
    CHECK(bq_config_valid(&c));
    bq_command_t cmd={.expected_version=1, .config_mask=BQ_CFG_HIGH|BQ_CFG_CLEAR,
        .config={.high_c=10, .clear_c=5}};
    CHECK(bq_config_patch(&c,&cmd,&next) && next.version==2 && next.high_c==10 && next.sample_interval_ms==1000);
    cmd.expected_version=2; CHECK(!bq_config_patch(&c,&cmd,&next));
    cmd.expected_version=1; cmd.config.clear_c=11; CHECK(!bq_config_patch(&c,&cmd,&next));
    CHECK(c.version==1 && c.high_c==8 && c.clear_c==6);
    c.dwell_ms=999; CHECK(!bq_config_valid(&c)); c=bq_default_config();
    c.high_c=NAN; CHECK(!bq_config_valid(&c)); c=bq_default_config();
    c.sample_interval_ms=60001; CHECK(!bq_config_valid(&c)); c=bq_default_config();
    CHECK(bq_deadline_valid(11000,10000));
    CHECK(bq_deadline_valid(40000,10000));
    CHECK(!bq_deadline_valid(40001,10000));
    CHECK(!bq_deadline_valid(10000,10000));
    CHECK(!bq_deadline_valid(9999,10000));
    CHECK(!bq_deadline_valid(UINT64_MAX,0));

    bq_rules_t r={0};
    CHECK(bq_rules_sample(&r,&c,true,8,0)==0 && !r.dwell_pending);
    CHECK(bq_rules_sample(&r,&c,true,10,1000)==0);
    CHECK(bq_rules_sample(&r,&c,true,10,5999)==0);
    CHECK(bq_rules_sample(&r,&c,true,10,6000)==BQ_HIGH_OPEN && r.high_episode==1);
    CHECK(bq_rules_sample(&r,&c,true,10,12000)==0);
    CHECK(bq_rules_sample(&r,&c,true,7,13000)==0 && r.high_active);
    CHECK(bq_rules_sample(&r,&c,true,6,14000)==0);
    CHECK(bq_rules_sample(&r,&c,false,NAN,15000)==0 && r.high_active && !r.dwell_pending);
    CHECK(bq_rules_sample(&r,&c,false,NAN,16000)==0);
    CHECK(bq_rules_sample(&r,&c,false,NAN,17000)==BQ_FAULT_OPEN && r.high_active);
    CHECK(bq_rules_sample(&r,&c,false,NAN,18000)==0);
    CHECK(bq_rules_sample(&r,&c,true,5,19000)==0);
    CHECK(bq_rules_sample(&r,&c,true,5,20000)==0);
    CHECK(bq_rules_sample(&r,&c,true,5,21000)==BQ_FAULT_CLEAR && r.high_active);
    CHECK(bq_rules_sample(&r,&c,true,5,23999)==0);
    CHECK(bq_rules_sample(&r,&c,true,5,24000)==BQ_HIGH_CLEAR && !r.high_active);
    CHECK(bq_rules_sample(&r,&c,true,5,30000)==0);
    CHECK(bq_rules_sample(&r,&c,true,10,31000)==0);
    CHECK(bq_rules_sample(&r,&c,true,7,35000)==0 && !r.dwell_pending);
    CHECK(bq_rules_sample(&r,&c,true,10,36000)==0);
    c.version++; CHECK(bq_rules_sample(&r,&c,true,10,40000)==0);
    CHECK(bq_rules_sample(&r,&c,true,10,45000)==BQ_HIGH_OPEN && r.high_episode==2);
    c.version++; CHECK(bq_rules_sample(&r,&c,true,7,46000)==0 && r.high_active);

    CHECK(bq_parse_command(valid_on,strlen(valid_on),&cmd)==NULL && cmd.output_on && cmd.action==BQ_SET_OUTPUT);
    bq_command_t same;
    CHECK(bq_parse_command(valid_on,strlen(valid_on),&same)==NULL && !memcmp(&same,&cmd,sizeof(cmd)));
    CHECK(parse_modified("schema_version",cJSON_CreateNumber(2),&cmd)!=NULL);
    CHECK(parse_modified("schema_version",cJSON_CreateString("1"),&cmd)!=NULL);
    CHECK(parse_modified("expires_at_uptime_ms",cJSON_CreateNumber(1.5),&cmd)!=NULL);
    CHECK(parse_modified("device_id",cJSON_CreateString("bad/device"),&cmd)!=NULL);
    CHECK(parse_modified("target_session_id",cJSON_CreateNull(),&cmd)!=NULL);
    CHECK(parse_modified("action",cJSON_CreateString("toggle"),&cmd)!=NULL);
    CHECK(parse_modified("params",cJSON_CreateString("ON"),&cmd)!=NULL);
    CHECK(parse_modified("params",cJSON_Parse("{\"channel\":\"relay-1\",\"state\":\"toggle\"}"),&cmd)!=NULL);
    CHECK(parse_modified("params",cJSON_Parse("{\"channel\":\"relay-2\",\"state\":\"ON\"}"),&cmd)!=NULL);
    CHECK(parse_modified("params",cJSON_Parse("{\"channel\":\"relay-1\",\"state\":\"ON\",\"extra\":1}"),&cmd)!=NULL);
    CHECK(parse_modified("params",cJSON_Parse("{\"channel\":\"relay-1\",\"state\":\"ON\",\"state\":\"OFF\"}"),&cmd)!=NULL);
    CHECK(bq_parse_command("{} garbage",10,&cmd)!=NULL);
    CHECK(bq_parse_command("{",1,&cmd)!=NULL);
    const char nul[]="{}\0hidden"; CHECK(bq_parse_command(nul,sizeof(nul)-1,&cmd)!=NULL);
    const char *deep="[[[[[[[[[0]]]]]]]]]"; CHECK(bq_parse_command(deep,strlen(deep),&cmd)!=NULL);
    const char *unicode="{\"command_id\":\"x\\u0000evil\"}"; CHECK(bq_parse_command(unicode,strlen(unicode),&cmd)!=NULL);

    cJSON *j=cJSON_Parse(valid_on);
    cJSON_ReplaceItemInObject(j,"action",cJSON_CreateString("set_config"));
    cJSON_ReplaceItemInObject(j,"params",cJSON_Parse("{\"expected_config_version\":1,\"high_c\":12,\"telemetry_interval_ms\":3000}"));
    char *json=cJSON_PrintUnformatted(j);
    CHECK(bq_parse_command(json,strlen(json),&cmd)==NULL);
    CHECK(cmd.config_mask==(BQ_CFG_HIGH|BQ_CFG_TELEMETRY) && cmd.expected_version==1);
    cJSON_free(json);
    cJSON_ReplaceItemInObject(j,"params",cJSON_Parse("{\"expected_config_version\":1,\"sample_interval_ms\":999}"));
    json=cJSON_PrintUnformatted(j); CHECK(bq_parse_command(json,strlen(json),&cmd)!=NULL); cJSON_free(json);
    cJSON_ReplaceItemInObject(j,"params",cJSON_Parse("{\"expected_config_version\":1}"));
    json=cJSON_PrintUnformatted(j); CHECK(bq_parse_command(json,strlen(json),&cmd)!=NULL); cJSON_free(json);
    cJSON_ReplaceItemInObject(j,"action",cJSON_CreateString("get_status"));
    cJSON_ReplaceItemInObject(j,"params",cJSON_CreateObject());
    json=cJSON_PrintUnformatted(j); CHECK(bq_parse_command(json,strlen(json),&cmd)==NULL && cmd.action==BQ_GET_STATUS); cJSON_free(json);
    cJSON_AddStringToObject(j,"extra","not allowed");
    json=cJSON_PrintUnformatted(j); CHECK(bq_parse_command(json,strlen(json),&cmd)!=NULL); cJSON_free(json); cJSON_Delete(j);

    bq_assembly_t a={0}; int total=(int)strlen(valid_on);
    CHECK(bq_assemble(&a,valid_on,20,0,total,true,false,100)==0);
    CHECK(bq_assemble(&a,valid_on+20,total-20,20,total,false,false,101)==1);
    CHECK(!strcmp(a.payload,valid_on) && !a.active);
    CHECK(bq_assemble(&a,valid_on,total,0,total,true,true,200)==1 && a.retained);
    CHECK(bq_assemble(&a,valid_on,20,0,total,true,false,300)==0);
    CHECK(bq_assemble(&a,valid_on+20,total-20,21,total,false,false,301)==-1);
    CHECK(bq_assemble(&a,valid_on,20,0,total,true,false,400)==0);
    CHECK(bq_assemble(&a,valid_on+20,total-20,20,total,false,false,2401)==-1);
    CHECK(bq_assemble(&a,valid_on,1,0,2049,true,false,3000)==-1);
    CHECK(bq_assemble(&a,valid_on,total,0,total,false,false,3000)==-1);
    CHECK(bq_assemble(&a,valid_on,20,0,total,true,false,3000)==0);
    CHECK(bq_assemble(&a,valid_on,total,0,total,true,false,3001)==1);
    printf("PASS: %u checks (config, deadlines, notification episodes, JSON validation, fragments)\n",checks);
    return 0;
}
