#include "camera_review_common.h"
#define PAGE_MAX 9
#define CAMERA_SLOT 8
#define CAMERA_VIEW_Y 92
#define CAMERA_VIEW_H 135
#define LV_OBJ_FLAG_HIDDEN 1
typedef struct { int kind; } lv_obj_t;
static lv_obj_t *s_pages[PAGE_MAX];
static bool s_cam_had_image, s_dirty;
static int s_midx, live_pages, created, stopped, started;
static bool camera_stopping;
static lv_obj_t *page_new(void) {
    // A cancelled worker may still hold its preview. Only one detail page fits
    // the remaining budget, rather than hidden pages plus all four AMS pages.
    assert(live_pages == 0);
    lv_obj_t *page = calloc(1, sizeof(*page)); assert(page);
    page->kind = -1; live_pages++; created++;
    return page;
}
static void lv_obj_delete(lv_obj_t *page) {
    if (page->kind == CAMERA_SLOT) assert(camera_stopping);
    free(page); live_pages--;
}
static void lv_obj_remove_flag(lv_obj_t *page, int flag) { assert(page); }
static void app_camera_start(int machine, int x, int y, int w, int h) {
    assert(x == 0 && y == 92 && w == 240 && h == 135);
    camera_stopping = false; started++;
}
static void app_camera_stop(void) { camera_stopping = true; stopped++; }
static void page_camera_build(lv_obj_t *p) { p->kind=8; }
static void page_progress_build(lv_obj_t *p) { p->kind=0; }
static void page_temp_build(lv_obj_t *p) { p->kind=1; }
static void page_ams_build(lv_obj_t *p, int index) { assert(index>=0 && index<4); p->kind=index+2; }
static void page_device_build(lv_obj_t *p) { p->kind=6; }
/* @PAGES@ */
int main(int argc, char **argv) {
    const int sequence[] = {0,8,1,2,3,4,5,6,8,0};
    for (int loop = 0; loop < 50; loop++) for (unsigned i = 0; i < sizeof(sequence)/sizeof(*sequence); i++) {
        int show = sequence[i], prior_created = created; apply_page_vis(show);
        assert(live_pages == 1 && s_pages[show] && s_pages[show]->kind == show);
        if (created != prior_created) assert(s_dirty);
        for (int page = 0; page < PAGE_MAX; page++) assert((s_pages[page] != NULL) == (page == show));
        int before = created; s_dirty = false; apply_page_vis(show);
        assert(created == before && !s_dirty);
    }
    int before = created; apply_page_vis(-1); apply_page_vis(7); apply_page_vis(PAGE_MAX);
    assert(created == before && live_pages == 1 && started == stopped);
    lv_obj_delete(s_pages[0]); assert(live_pages == 0);
    return 0;
}
