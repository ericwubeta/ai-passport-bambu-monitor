#include "../main/app_telemetry.h"
#include "../main/spirit_bindings.h"
#include "../main/spirit_dispatch.h"
#include "../main/spirit_event_logic.h"
#include "../main/spirit_adpcm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void merge(bambu_state_t *s,const char *json,uint64_t now)
{
    app_telemetry_merge(s,json,(int)strlen(json),now);
}

static void telemetry(void)
{
    bambu_state_t s={0};
    merge(&s,"{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":0,\"mc_remaining_time\":0,\"device\":{\"extruder\":{\"state\":274,\"info\":[{\"id\":1,\"hnow\":7,\"temp\":33},{\"id\":0,\"hnow\":8,\"temp\":34}]}}}}",100);
    assert(s.heads.count==2 && s.structured_heads && s.extruder.current_id==1);
    assert(nozzle_head_active(&s.heads,s.extruder)->temperature.current_c==33);
    assert(nozzle_head_find(&s.heads,0)->nozzle_id==8);
    assert(s.pct_valid && s.pct==0 && s.remain_valid && s.remain_min==0);
    merge(&s,"{\"print\":{\"device\":{\"extruder\":{\"state\":4082,\"info\":[{\"id\":0,\"temp\":14418135},{\"id\":1,\"hnow\":2}]}}}}",200);
    assert(!s.extruder.current_known && !s.extruder.target_known);
    assert(nozzle_head_find(&s.heads,0)->temperature.current_c==215);
    assert(nozzle_head_find(&s.heads,0)->temperature.target_c==220);
    assert(nozzle_head_find(&s.heads,1)->updated_ms==100);
    assert(s.pct_ms==100 && s.remain_ms==100);
    merge(&s,"{\"print\":{\"device\":{\"extruder\":{\"info\":[{\"id\":0,\"temp\":4294967296},{\"id\":1,\"temp\":1.5}]}}}}",300);
    assert(nozzle_head_find(&s.heads,0)->updated_ms==200 && nozzle_head_find(&s.heads,1)->updated_ms==100);
    merge(&s,"{\"print\":{\"nozzle_temper\":0,\"bed_temper\":0,\"chamber_temper\":0}}",400);
    assert(s.nozzle_valid && s.bed_valid && s.chamber_valid);
    merge(&s,"{\"print\":{\"gcode_state\":\"FINISH\"}}",500);
    assert(s.nozzle_valid && s.nozzle_ms==400 && s.bed_ms==400);
    assert(app_telemetry_fresh(true,400,90400) && !app_telemetry_fresh(true,400,90401));
    app_telemetry_invalidate(&s);
    assert(!s.nozzle_valid && !s.pct_valid && !nozzle_head_find(&s.heads,0)->temperature_valid);
    merge(&s,"{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":10",600);
    assert(!s.pct_valid); // A partial packet cannot apply even its early fields.
    merge(&s,"{\"info\":{\"module\":[{\"project_name\":\"N6\"}]}}",700);
    assert(!strcmp(s.model,"N6"));
    // Actual X2D get_version reply (ha-bambulab fixture): product_name carries
    // the marketing name; project_name was not the field the printer sends.
    merge(&s,"{\"info\":{\"module\":[{\"name\":\"ota\",\"product_name\":\"Bambu Lab X2D\"}]}}",710);
    assert(!strcmp(s.model,"X2D"));
    merge(&s,"{\"info\":{\"module\":[{\"product_name\":\"AMS 2 Pro (1)\"}]}}",720);
    assert(!strcmp(s.model,"X2D"));
    merge(&s,"{\"print\":{\"module\":[{\"product_name\":\"Bambu Lab X1-Carbon\"}]}}",730);
    assert(!strcmp(s.model,"X1C"));
    merge(&s,"{\"print\":{\"mc_percent\":17,\"subtask_name\":\"escaped \\\"part\\\"\"}}\n\r\t ",800);
    assert(s.pct_valid && s.pct==17 && s.pct_ms==800);
}

static void materials(void)
{
    bambu_state_t s={0};
    merge(&s,"{\"print\":{\"ams\":{\"ams_exist_bits\":5,\"tray_now\":9,\"ams\":[{\"id\":2,\"humidity_raw\":42,\"tray\":[{\"id\":1,\"tray_type\":\"PLA\",\"tray_color\":\"000000FF\",\"remain\":80,\"tray_weight\":\"1000\"}]},{\"id\":0,\"humidity\":2,\"tray\":[{\"id\":3,\"tray_type\":\"PETG\",\"remain\":-1}]}]}}}",1000);
    assert(s.ams_count==2 && s.ams[0].id==2 && s.ams[1].id==0);
    assert(s.ams[0].tray[1].color_valid && s.ams[0].tray[1].active && s.ams[0].tray[1].tray_remain==800);
    assert(s.ams[1].tray[3].is_used && !s.ams[1].tray[3].pct_valid);
    merge(&s,"{\"print\":{\"ams\":{\"ams\":[{\"id\":0,\"tray\":[{\"id\":3,\"remain\":20}]}]}}}",2000);
    assert(s.ams_count==2 && s.ams[0].id==2 && s.ams[0].humidity_raw_updated_ms==1000);
    assert(s.ams[0].tray[1].updated_ms==1000 && s.ams[0].tray[1].pct_updated_ms==1000);
    assert(!strcmp(s.ams[1].tray[3].tray_type,"PETG") && s.ams[1].tray[3].tray_pct==20);
    merge(&s,"{\"print\":{\"ams\":{\"tray_now\":255}}}",3000);
    assert(!s.ams_active && !s.ams[0].tray[1].active);
    merge(&s,"{\"print\":{\"ams\":{\"ams_exist_bits\":1}}}",4000);
    assert(s.ams_count==1 && s.ams[0].id==0);
    merge(&s,"{\"print\":{\"ams\":{\"ams_exist_bits\":0},\"vt_tray\":{\"tray_type\":\"PLA\",\"tray_color\":\"000000FF\",\"remain\":50,\"tray_weight\":1000}}}",5000);
    assert(s.ams_count==0 && s.ams[0].tray[0].tray_remain==500 && s.ams[0].tray[0].color_valid);
    merge(&s,"{\"print\":{\"mc_percent\":40}}",6000);
    assert(s.ams[0].tray[0].tray_remain==500 && s.ams[0].tray[0].pct_updated_ms==5000);
    merge(&s,"{\"print\":{\"vt_tray\":{\"remain\":40}}}",7000);
    assert(s.ams[0].tray[0].tray_remain==400 && !strcmp(s.ams[0].tray[0].tray_type,"PLA"));
}

static spirit_status_t status(const char *state)
{
    return (spirit_status_t){.cloud_online=true,.state_received=true,.state_updated_ms=1000,.gcode_state=state};
}

static void dispatch_and_events(void)
{
    spirit_dispatch_status_t s[4]={{0}};
    for(int i=0;i<4;i++) s[i].status=status("RUNNING");
    spirit_dispatch_t d={0};
    assert(spirit_dispatch_choose(&d,s,4,1000)==0);
    assert(spirit_dispatch_choose(&d,s,4,8999)==0);
    assert(spirit_dispatch_choose(&d,s,4,9000)==1);
    s[3].status.gcode_state="PAUSE";
    assert(spirit_dispatch_choose(&d,s,4,9001)==3);
    s[1].status.gcode_state="PAUSE";
    assert(spirit_dispatch_choose(&d,s,4,17001)==1);
    assert(spirit_dispatch_choose(&d,s,4,25001)==3);
    s[2].status.gcode_state="FAILED";
    assert(spirit_dispatch_choose(&d,s,4,25002)==2);
    assert(spirit_model_family("C11")==2 && spirit_model_family("P1P")==2);
    assert(spirit_model_family("N2S")==0 && spirit_model_family("N6")==1 && spirit_model_family("unknown")==2);
    spirit_event_latch_t l={0};spirit_status_t v=status("FINISH");
    assert(spirit_event_observe(&l,&v,0,false,1000)==-1);
    l=(spirit_event_latch_t){0};
    assert(spirit_event_observe(&l,&v,11,true,1000)==-1);
    l=(spirit_event_latch_t){0};
    assert(spirit_event_observe(&l,&v,11,false,1000)==2);
    assert(spirit_event_observe(&l,&v,11,false,1001)==-1);
    v.gcode_state="IDLE";assert(spirit_event_observe(&l,&v,11,false,1002)==-1);
    v.gcode_state="FINISH";assert(spirit_event_observe(&l,&v,11,false,1003)==-1);
    v.job_generation++;assert(spirit_event_observe(&l,&v,12,false,1004)==2);
    v.gcode_state="PAUSE";assert(spirit_event_observe(&l,&v,12,false,1005)==1);
    v.gcode_state="RUNNING";(void)spirit_event_observe(&l,&v,12,false,1006);
    v.gcode_state="PAUSE";assert(spirit_event_observe(&l,&v,12,false,61006)==1);
    v.print_error=0x03008015;assert(spirit_event_observe(&l,&v,12,false,61007)==4);
}

static void bindings(void)
{
    spirit_bindings_t b={.version=1};
    spirit_binding_t *a=spirit_binding_ensure(&b,"serial-A",2);
    spirit_binding_t *z=spirit_binding_ensure(&b,"serial-Z",2);
    assert(a && z && spirit_bindings_valid(&b));a->skin=3;z->skin=0;
    assert(spirit_binding_ensure(&b,"serial-Z",1)->skin==0);
    assert(spirit_binding_ensure(&b,"serial-A",1)->skin==3);
    a->finish_token=spirit_task_token("job-123");a->finish_ack=1;
    spirit_bindings_t reboot=b;
    assert(spirit_bindings_valid(&reboot) && spirit_binding_find(&reboot,"serial-A")->finish_ack);
    assert(spirit_task_token("job-123")!=spirit_task_token("job-124") && spirit_task_token("0")==0);
    for(int i=2;i<8;i++) { char sn[16];snprintf(sn,sizeof(sn),"serial-%d",i);assert(spirit_binding_ensure(&b,sn,0)); }
    assert(!spirit_binding_ensure(&b,"ninth",0));
    assert(spirit_binding_find(&b,"serial-A")->skin==3);
    memset(b.entries[0].serial,'x',sizeof(b.entries[0].serial));assert(!spirit_bindings_valid(&b));
}

static void adpcm(void)
{
    spirit_adpcm_t state={0};
    assert(spirit_adpcm_decode(&state,7)==11);
    assert(spirit_adpcm_decode(&state,15)==-19);
    for(int i=0;i<100;i++) spirit_adpcm_decode(&state,7);
    assert(state.predictor==32767 && state.index==88);
    for(int i=0;i<100;i++) spirit_adpcm_decode(&state,15);
    assert(state.predictor==-32768 && state.index==88);
}

int main(int argc,char **argv)
{
    telemetry();materials();dispatch_and_events();bindings();adpcm();
    if(argc>1) {
        FILE *input=fopen(argv[1],"rb");assert(input);char data[8192];
        size_t bytes=fread(data,1,sizeof(data),input);fclose(input);assert(bytes<sizeof(data));
        bambu_state_t state={0};app_telemetry_merge(&state,data,(int)bytes,1000);
        fprintf(stderr,"Fixture: state=%s heads=%zu current=%u pct_valid=%d chamber=%d bytes=%zu\n",state.gcode_state,state.heads.count,state.extruder.current_id,state.pct_valid,state.chamber_t,bytes);
        assert(state.heads.count==2 && state.extruder.current_id==1);
        assert(nozzle_head_find(&state.heads,0)->temperature.current_c==34);
        assert(nozzle_head_find(&state.heads,1)->temperature.current_c==33);
        assert(state.chamber_valid && state.chamber_t==27 && state.pct_valid && state.pct==100);
    }
    printf("PASS telemetry/delta/AMS/identities/dispatch/events/ADPCM; snapshot=%zu bytes\n",sizeof(bambu_state_t));
    return 0;
}
