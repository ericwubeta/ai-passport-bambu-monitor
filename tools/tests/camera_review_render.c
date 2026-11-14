#include "camera_review_common.h"
#include "app_camera.h"
#include "app_camera_config.h"
#include "camera_geometry.h"
#define JSON_LIMIT 1024
#define CONFIG_CAMERA_BUFFERED_PREVIEW 1
#define MALLOC_CAP_32BIT 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_DMA 8
typedef void esp_tls_t;
typedef struct { int unused; } mbedtls_md5_context;
typedef unsigned UINT;
typedef uint8_t BYTE;
typedef struct { int left, right, top, bottom; } JRECT;
typedef struct { unsigned width, height, msy; void *device; } JDEC;
typedef int JRESULT;
enum { JDR_OK, JDR_MEM1, JDR_MEM2, JDR_INP };
/* @READER@ */
static bool active = true, verified, decoding, locked, ended;
static int draws, allocated, fail_after = -1;
static const char *scenario;
static uint16_t screen[240 * 135];
static camera_reader_t *reader;
static bool current(uint32_t token) { return active && token == 7; }
static void *allocate_test(size_t n) {
    int index = allocated++;
    if (fail_after >= 0 && index == fail_after) return NULL;
    return malloc(n);
}
static void *heap_caps_malloc(size_t n, unsigned caps) { return allocate_test(n); }
static void *heap_caps_calloc(size_t n, size_t size, unsigned caps) { return calloc(n, size); }
static size_t heap_caps_get_free_size(unsigned caps) { return !strcmp(scenario,"low-memory") ? 10000 : 70000; }
static bool bsp_lvgl_lock(int timeout) {
    if (!strcmp(scenario,"busy")) return false;
    locked = true; return true;
}
static void bsp_lvgl_unlock(void) { locked = false; }
static bool bsp_lvgl_stream_set(int y, int h, uint32_t token) { assert(locked); return true; }
static void bsp_lvgl_stream_end(uint32_t token) { ended = true; }
static bool bsp_lvgl_stream_draw(uint32_t token, int x, int y, int w, int h, const uint16_t *pixels) {
    assert(verified && !decoding && locked && current(token));
    assert(x == 0 && y >= 50 && w == 240 && h <= CAMERA_FRAME_ROWS && y + h <= 185);
    draws++;
    if (!strcmp(scenario,"draw-failure")) return false;
    memcpy(screen + (y - 50) * 240, pixels, w * h * 2);
    return true;
}
void app_camera_snapshot(camera_state_t *state) { *state = (camera_state_t){.has_image=true}; }
static void mbedtls_md5_init(mbedtls_md5_context *ctx) {}
static void mbedtls_md5_starts(mbedtls_md5_context *ctx) {}
static void mbedtls_md5_free(mbedtls_md5_context *ctx) {}
static void mbedtls_md5_finish(mbedtls_md5_context *ctx, uint8_t *md5) { memset(md5, 0, 16); verified = true; }
static UINT jpeg_input(JDEC *decoder, BYTE *destination, UINT requested) { return 0; }
/* @DRAW@ */
/* @ALLOCATE@ */
/* @PRESENT@ */
/* @OUTPUT@ */
static JRESULT jd_prepare(JDEC *decoder, void *input, void *work, unsigned size, void *device) {
    decoder->width = !strcmp(scenario,"portrait") ? 320 : !strcmp(scenario,"small") ? 9 : 640;
    decoder->height = !strcmp(scenario,"portrait") ? 640 : !strcmp(scenario,"small") ? 7 : 480;
    decoder->msy = 1; decoder->device = device;
    if (!strcmp(scenario,"cancel-header")) active = false;
    return JDR_OK;
}
static JRESULT jd_decomp(JDEC *decoder, UINT (*output)(JDEC *, void *, JRECT *), unsigned scale) {
    decoding = true;
    int w = decoder->width >> scale, h = decoder->height >> scale;
    uint8_t rgb[8 * 8 * 3];
    for (int i = 0; i < 64; i++) { rgb[i*3] = 255; rgb[i*3+1] = rgb[i*3+2] = 0; }
    for (int y = 0; y < h; y += 8) for (int x = 0; x < w; x += 8) {
        JRECT rect = {x, x + 7 < w ? x + 7 : w-1, y, y + 7 < h ? y+7 : h-1};
        if (!output(decoder, rgb, &rect)) { decoding = false; return JDR_INP; }
        assert(draws == 0); // No partial image may escape while decoding.
    }
    decoding = false;
    if (!strcmp(scenario,"cancel")) active = false;
    if (!strcmp(scenario,"input-failure")) reader->failed = true;
    return !strcmp(scenario,"decode-failure") ? JDR_INP : JDR_OK;
}
/* @DECODE@ */
int main(int argc, char **argv) {
    assert(argc == 2); scenario = argv[1];
    camera_reader_t r = {.token=7, .y=50, .vw=240, .vh=135, .received=4, .total=4};
    reader = &r;
    memset(r.md5_expected, '0', 32);
    if (!strcmp(scenario,"checksum")) r.md5_expected[0] = '1';
    if (!strcmp(scenario,"allocation")) fail_after = 6;
    for (unsigned i = 0; i < sizeof(screen)/sizeof(*screen); i++) screen[i] = 0xdead;
    bool ok = decode(&r);
    bool success = !strcmp(scenario,"landscape") || !strcmp(scenario,"portrait") || !strcmp(scenario,"small") || !strcmp(scenario,"rate");
    assert(ok == success && !locked && r.stripe == NULL);
    if (success) {
        assert(draws == 17 && r.drew && !ended);
        for (int y = 0; y < 135; y++) for (int x = 0; x < 240; x++) {
            bool image = x >= r.geometry.x && x < r.geometry.x + r.geometry.width &&
                         y >= r.geometry.y && y < r.geometry.y + r.geometry.height;
            assert(screen[y*240+x] == (image ? 0x00f8 : 0));
        }
    } else if (!strcmp(scenario,"draw-failure")) assert(draws == 1 && r.drew && ended);
    else {
        assert(draws == 0 && !r.drew && !ended);
        for (unsigned i = 0; i < sizeof(screen)/sizeof(*screen); i++) assert(screen[i] == 0xdead);
    }
    camera_frame_free(&r.frame);
    if (!strcmp(scenario,"cancel-header")) assert(allocated == 0);
    if (!strcmp(scenario,"small")) {
        // Exercise packed output for an odd viewport width, across block and
        // final partial-block boundaries, with guards on both ends of DMA RAM.
        camera_frame_t frame = {0};
        assert(camera_frame_alloc(&frame, 9, 17, malloc));
        camera_frame_clear(&frame);
        for (int y = 0; y < 17; y++) for (int x = 0; x < 9; x++)
            camera_frame_pixel(&frame, x, y, (uint16_t)(y * 9 + x + 1));
        uint32_t guarded[43];
        for (int i = 0; i < 43; i++) guarded[i] = 0xdeadbeef;
        camera_frame_copy_rows(&frame, 6, 9, guarded + 1);
        const uint16_t *pixels = (const uint16_t *)(guarded + 1);
        assert(guarded[0] == 0xdeadbeef && guarded[42] == 0xdeadbeef);
        for (int i = 0; i < 81; i++) assert(pixels[i] == 6 * 9 + i + 1);
        assert(pixels[81] == 0xdead);
        camera_frame_free(&frame);
    }
    if (!strcmp(scenario,"rate")) {
        camera_rate_t rate = {0};
        camera_rate_record(&rate, 1000000); assert(camera_rate_tenths(&rate) == 0);
        camera_rate_record(&rate, 2000000); assert(camera_rate_tenths(&rate) == 10);
        camera_rate_record(&rate, 4000000); assert(camera_rate_tenths(&rate) == 7);
        camera_rate_record(&rate, 5000000); camera_rate_record(&rate, 6000000);
        camera_rate_record(&rate, 10000000); assert(rate.count == 4 && camera_rate_tenths(&rate) == 5);
        assert(!camera_rate_stale(10000000, 14999999, 5000));
        assert(camera_rate_stale(10000000, 15000000, 5000));
        assert(camera_rate_stale(0, 1000000, 5000));
        camera_rate_record(&rate, 1); assert(rate.count == 0 && camera_rate_tenths(&rate) == 0);
    }
    return 0;
}
