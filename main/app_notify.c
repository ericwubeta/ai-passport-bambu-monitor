// Fixed character phrases in Flash, decoded256 samples at a time.
#include "app_notify.h"
#include "app_settings.h"
#include "app_spirit_skins.h"
#include "spirit_adpcm.h"
#include "spirit_page_voice_assets.h"
#include "spirit_voice_assets.h"
#include "spirit_external_assets.h"
#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdint.h>

static atomic_uint s_busy;
static atomic_uint s_pending[4];
static atomic_uint s_epoch[4];
static unsigned s_cursor;
#define EPOCH_MASK 0x3fffffu

// Verbatim text of the 20 linked ADPCM clips
// (assets/images/print-spirit/v210-voice-v01/manifest.json). The bubble only
// displays this text; it does not synthesize or add audio.
static const char *const s_voice_text[4][5] = {
    {"我盯着呢，你先忙呀。", "嗯？怎么停住啦？帮我看看。", "完成啦，快来看呀！",
     "这次没完成，先看看哪里不对。", "明确缺料啦，帮我补充耗材呀。"},
    {"我在这里看着进度。", "暂停原因还不清楚，一起看看吧。", "打印完成了，可以验收啦。",
     "这次没有完成，我们先检查原因。", "打印机报告缺料，请补充耗材。"},
    {"状态已核对，继续观察。", "检测到暂停，请检查原因。", "打印完成，请验收。",
     "任务未完成，请检查告警。", "收到缺料报告，请补充耗材。"},
    {"我盯着呢，你忙你的。", "嗯？停住了，过来看看？", "完成了，去收下战果吧。",
     "没完成，先把原因找出来。", "缺料了，过来补点耗材吧。"},
};

// Scene-specific detail-page lines (v218-page-voice-v03), indexed by
// [role][page][variant] with page 0=温度 1=耗材 2=设备. Variant 1 is an
// alternate wording used on a later entry so repeats do not feel identical.
static const char *const s_page_text[4][3][2] = {
    {{"温度我盯着呀。", "温度记下了。"},
     {"耗材我记着呀。", "耗材记下了。"},
     {"连接我看着呀。", "信号盯住了。"}},
    {{"温度我看着呢。", "温度留意着。"},
     {"耗材我看着呢。", "耗材留意着。"},
     {"连接我看着呢。", "连接留意着。"}},
    {{"温度已核对。", "温度已确认。"},
     {"耗材已核对。", "耗材已确认。"},
     {"连接已核对。", "连接已确认。"}},
    {{"温度我盯着。", "温度看过了。"},
     {"耗材我盯着。", "耗材看过了。"},
     {"连接我盯着。", "网络看过了。"}},
};

static unsigned priority(unsigned request)
{
    if(!request) return 0;
    unsigned kind=(request>>5)&15;
    return kind==3 ? 5 : kind==4 ? 4 : kind==1 ? 3 : kind==2 ? 2 : 1;
}

static unsigned pending_priority(void)
{
    unsigned best=0;
    for(unsigned i=0;i<4;i++) { unsigned p=priority(atomic_load(&s_pending[i]));if(p>best)best=p; }
    return best;
}

static unsigned take_next(void)
{
    unsigned best=pending_priority();
    if(!best) return 0;
    for(unsigned j=1;j<=4;j++) {
        unsigned i=(s_cursor+j)%4;
        if(priority(atomic_load(&s_pending[i]))==best) {
            s_cursor=i;
            return atomic_exchange_explicit(&s_pending[i],0,memory_order_acquire);
        }
    }
    return 0;
}

static void play_clip(const spirit_voice_clip_t *clip, unsigned request, unsigned epoch);

static void speech_worker(void *argument)
{
    (void)argument;
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    for (;;) {
        unsigned request=take_next();
        if(!request) break;
        unsigned skin=request&7, machine=(request>>3)&3, kind=(request>>5)&15;
        unsigned epoch=request>>10;
        const spirit_voice_clip_t *clip;
        if (skin >= 4) {
            clip = spirit_external_voice(skin, kind);
        } else if (kind < 5) {
            clip = &spirit_voices[skin][kind];
        } else {
            unsigned index = kind - 5;          // page * 2 + variant
            clip = &spirit_page_voices[skin][index / 2][index % 2];
        }
        if (!clip) continue;
        if(epoch!=(atomic_load(&s_epoch[machine])&EPOCH_MASK)) continue;
        play_clip(clip, request, epoch);
        ESP_LOGI("spirit_voice","after: internal free=%u biggest=%u",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    atomic_store_explicit(&s_busy,0,memory_order_release);
    // Work racing teardown stays pending; the next LVGL poll retries it.
    vTaskDelete(NULL);
}

// Clips may carry their own rate (8kHz page hints) or 0 for the historical
// 12kHz event set; the codec is opened per clip so mixing rates is safe.
// A higher-priority request or an epoch bump stops playback mid-clip.
static void play_clip(const spirit_voice_clip_t *clip, unsigned request, unsigned epoch)
{
    uint16_t rate = clip->rate ? clip->rate : 12000;
    ESP_LOGI("spirit_voice","before: internal free=%u biggest=%u",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if(bsp_audio_set_format(rate,16,1)!=ESP_OK) return;
    bsp_audio_set_volume((uint8_t)app_settings_volume());
    spirit_adpcm_t state={0}; int16_t buffer[256];
    for(uint32_t at=0; at<clip->samples;) {
        unsigned machine=(request>>3)&3;
        if(epoch!=(atomic_load(&s_epoch[machine])&EPOCH_MASK) || pending_priority()>priority(request)) break;
        unsigned count=clip->samples-at>256 ? 256 : clip->samples-at;
        for(unsigned i=0; i<count; i++,at++) {
            unsigned packed=clip->data[at/2];
            buffer[i]=spirit_adpcm_decode(&state,(at&1) ? packed>>4 : packed&15);
        }
        if(bsp_audio_write(buffer,count*sizeof(buffer[0]))!=ESP_OK) break;
    }
    vTaskDelay(pdMS_TO_TICKS(30)); bsp_audio_close();
}

void app_notify_init(void) { } // No permanently reserved8KiB idle task.

void app_notify_poll(void)
{
    if(!pending_priority() || app_spirit_bindings_busy()) return;
    if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 8192 ||
       heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 12288) return;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong(&s_busy,&expected,1)) return;
    TaskHandle_t task=NULL;
    if(xTaskCreate(speech_worker,"spirit_voice",8192,NULL,3,&task)!=pdPASS) {
        atomic_store(&s_busy,0); return;
    }
    xTaskNotifyGive(task);
}

void app_notify_epoch(int machine)
{
    if(machine>=0 && machine<4) {
        atomic_fetch_add(&s_epoch[machine],1);
        atomic_store(&s_pending[machine],0);
    }
}

bool app_notify_busy(void)
{
    return atomic_load_explicit(&s_busy,memory_order_acquire) != 0;
}

const char *app_notify_voice_text(unsigned skin, unsigned kind)
{
    if (skin >= 4) return kind < 5 ? spirit_external_voice_text(skin, kind) : NULL;
    return kind < 5 ? s_voice_text[skin][kind] : NULL;
}

const char *app_notify_page_text(unsigned skin, unsigned page, unsigned variant)
{
    if (skin >= 4) return page < 3 && variant < 2 ? spirit_external_voice_text(skin, 5 + page * 2 + variant) : NULL;
    return page < 3 && variant < 2 ? s_page_text[skin][page][variant] : NULL;
}

void app_notify_page_voice(int machine, unsigned skin, unsigned page, unsigned variant)
{
    if (page >= 3 || variant >= 2) return;
    app_notify_voice(machine, skin, 5 + page * 2 + variant);
}

void app_notify_voice(int machine, unsigned skin, unsigned kind)
{
    if(machine<0 || machine>=4 || skin>=SPIRIT_SKIN_COUNT || kind>10) return;
    if (skin >= 4 && !spirit_external_voice(skin, kind)) return;
    // 3 role bits + 2 printer bits + 4 kind bits + valid bit + 22 epoch bits.
    unsigned request=0x200u|skin|((unsigned)machine<<3)|(kind<<5)|((atomic_load(&s_epoch[machine])&EPOCH_MASK)<<10);
    unsigned pending=atomic_load(&s_pending[machine]);
    if(pending && priority(pending)>priority(request)) return;
    atomic_store_explicit(&s_pending[machine],request,memory_order_release);
    app_notify_poll();
}

void app_notify_trigger(notify_kind_t kind)
{
    if(kind>=NOTIFY_FINISH && kind<=NOTIFY_PAUSE) {
        unsigned voice_kind=kind==NOTIFY_FINISH ? 2 : kind==NOTIFY_FAIL ? 3 : 1;
        app_notify_voice(0,app_spirit_skin_for_machine(0),voice_kind);
    }
}
