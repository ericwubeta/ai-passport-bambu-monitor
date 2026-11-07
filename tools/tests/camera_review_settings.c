#include "camera_review_common.h"
#include "app_config.h"
#include "app_camera_config.h"
static device_config_t nvs_config;
static bool load_ok = true;
static const char *legacy_serial = "TESTLEGACY00001";
#define CFG_BAMBU_SERIAL legacy_serial
bool config_load(device_config_t *out) { *out = nvs_config; return load_ok; }
/* @ROSTER@ */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define SBUF 1400
static char s_buf[SBUF], saved_serial[34];
static camera_config_t saved_camera;
static int saves, restarts;
typedef struct { int content_len; const char *body; char response[SBUF]; } httpd_req_t;
static int read_body(httpd_req_t *req, char *out, int size) { strlcpy(out,req->body,size); return req->content_len; }
static void send_json(httpd_req_t *req, const char *json) { strlcpy(req->response,json,sizeof(req->response)); }
static void schedule_restart(int ms) { restarts++; }
bool camera_config_load(const char *serial, camera_config_t *out) { memset(out,0,sizeof(*out)); return false; }
bool camera_config_save(const char *serial, const camera_config_t *cfg) {
    saves++; strlcpy(saved_serial,serial,sizeof(saved_serial)); saved_camera=*cfg; return true;
}
/* @HELPERS@ */
/* @GET@ */
/* @POST@ */
int main(int argc, char **argv) {
    assert(argc==2); const char *scenario=argv[1];
    int index=0; const char *expected=legacy_serial;
    if (!strcmp(scenario,"nvs") || !strcmp(scenario,"sparse")) {
        index=!strcmp(scenario,"sparse") ? 2 : 0; expected="TESTNVS00002";
        strlcpy(nvs_config.bambu_sn[index],expected,sizeof(nvs_config.bambu_sn[index]));
        strlcpy(nvs_config.bambu_name[index],"Printer A",sizeof(nvs_config.bambu_name[index]));
    } else if (!strcmp(scenario,"placeholder")) {
        device_config_t effective;
        legacy_serial="00C00AAAA000000"; assert(config_load_printers(&effective));
        assert(config_machine_count(&effective)==0);
        legacy_serial=""; assert(config_load_printers(&effective)); assert(config_machine_count(&effective)==0);
        httpd_req_t req={0}; assert(camera_get_handler(&req)==ESP_OK);
        assert(!strcmp(req.response,"{\"machines\":[]}")); return 0;
    } else if (!strcmp(scenario,"load-error")) load_ok=false;
    device_config_t effective;
    assert(config_load_printers(&effective)==load_ok);
    assert(!strcmp(effective.bambu_sn[index],expected));
    assert(index==0 || !effective.bambu_sn[0][0]);
    httpd_req_t get={0}; assert(camera_get_handler(&get)==ESP_OK);
    char selected[24]; snprintf(selected,sizeof(selected),"\"index\":%d",index);
    assert(strstr(get.response,selected)); assert(!strstr(get.response,"TEST1234"));
    assert(!strstr(get.response,"\"cert\"") && !strstr(get.response,"\"code\""));
    char body[160]; snprintf(body,sizeof(body),"idx=%d&host=printer.test&code=TEST1234&model=Test&mode=1&pair=0",index);
    httpd_req_t post={.content_len=(int)strlen(body),.body=body};
    assert(camera_save_handler(&post)==ESP_OK && !strcmp(post.response,"{\"ok\":1}"));
    assert(saves==1 && restarts==1 && !strcmp(saved_serial,expected));
    assert(!strcmp(saved_camera.access_code,"TEST1234") && !strcmp(saved_camera.host,"printer.test"));
    if (!strcmp(scenario,"legacy")) assert(config_machine_count(&nvs_config)==0); // No account migration/write.
    return 0;
}
