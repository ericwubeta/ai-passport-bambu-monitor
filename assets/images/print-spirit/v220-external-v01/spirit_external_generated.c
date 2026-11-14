// Generated from accepted native frames and authorized clone outputs.
#include "spirit_external_assets.h"
#include <string.h>
#include <stdint.h>
#define PACK_BYTES 1029832u
#define PACK_CRC 740974170u
#define FRAME_COUNT 72u
#define VOICE_COUNT 22u
static lv_image_dsc_t s_frames[FRAME_COUNT];
static spirit_voice_clip_t s_voices[2][11];
static bool s_ready;
static const uint32_t s_offsets[FRAME_COUNT]={32,6368,12704,19040,25376,31712,38048,44384,50720,57056,63392,69728,76064,82400,88736,95072,101408,107744,114080,120416,126752,133088,139424,145760,152096,158432,164768,171104,177440,183776,190112,196448,202784,209120,215456,221792,228128,234464,240800,247136,253472,259808,266144,272480,278816,285152,291488,297824,304160,310496,316832,323168,329504,335840,342176,348512,354848,361184,367520,373856,380192,386528,392864,399200,405536,411872,418208,424544,430880,437216,443552,449888};
static const spirit_device_clip_t s_clips[2][3][4]={
{
{
{.frames={&s_frames[0],&s_frames[1]},.timeline={0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[2],&s_frames[3],&s_frames[4],&s_frames[5]},.timeline={0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0},.loop=true},
{.frames={&s_frames[6],&s_frames[7]},.timeline={0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[8],&s_frames[9],&s_frames[10],&s_frames[11]},.timeline={1,1,1,0,0,0,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},.loop=false},
},
{
{.frames={&s_frames[12],&s_frames[13]},.timeline={0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[14],&s_frames[15],&s_frames[16],&s_frames[17]},.timeline={0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0},.loop=true},
{.frames={&s_frames[18],&s_frames[19]},.timeline={0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[20],&s_frames[21],&s_frames[22],&s_frames[23]},.timeline={1,1,1,0,0,0,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},.loop=false},
},
{
{.frames={&s_frames[24],&s_frames[25]},.timeline={0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[26],&s_frames[27],&s_frames[28],&s_frames[29]},.timeline={0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0},.loop=true},
{.frames={&s_frames[30],&s_frames[31]},.timeline={0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[32],&s_frames[33],&s_frames[34],&s_frames[35]},.timeline={1,1,1,0,0,0,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},.loop=false},
},
},
{
{
{.frames={&s_frames[36],&s_frames[37]},.timeline={0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[38],&s_frames[39],&s_frames[40],&s_frames[41]},.timeline={0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0},.loop=true},
{.frames={&s_frames[42],&s_frames[43]},.timeline={0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[44],&s_frames[45],&s_frames[46],&s_frames[47]},.timeline={1,1,1,0,0,0,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},.loop=false},
},
{
{.frames={&s_frames[48],&s_frames[49]},.timeline={0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[50],&s_frames[51],&s_frames[52],&s_frames[53]},.timeline={0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0},.loop=true},
{.frames={&s_frames[54],&s_frames[55]},.timeline={0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[56],&s_frames[57],&s_frames[58],&s_frames[59]},.timeline={1,1,1,0,0,0,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},.loop=false},
},
{
{.frames={&s_frames[60],&s_frames[61]},.timeline={0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[62],&s_frames[63],&s_frames[64],&s_frames[65]},.timeline={0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0},.loop=true},
{.frames={&s_frames[66],&s_frames[67]},.timeline={0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},.loop=true},
{.frames={&s_frames[68],&s_frames[69],&s_frames[70],&s_frames[71]},.timeline={1,1,1,0,0,0,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3},.loop=false},
},
},
};
static const char *const s_text[2][11]={
{"这场打印还在继续。我在旁边看着，有变化就告诉你。","咦？演出怎么暂停了？原因还不清楚，来看看设备提示。","谢幕时刻到！打印完成，去看看这次的作品吧。","这场打印没有完成。先看设备提示，我们再想办法。","设备报告缺料了。先看看供料提示，再安排下一幕。","温度记录在这里，请看实际数值。","温度这一幕，就按上报来核对。","耗材信息到场，请看实际用料。","线轴点名啦，实际信息在这里。","设备信息到了，请看连接记录。","巡场记录就位，连接状态在这里。"},
{"打印还在忙呢。你先忙你的，有新变化我再叫你。","嗯？怎么暂停啦？先别猜原因，来看看设备提示。","叮咚，打印完成！快去看看新鲜出炉的小作品吧。","这次打印没完成。先看看设备提示，咱们再想办法。","设备报告缺料啦。来，先看看供料提示。","温度小抄来啦，看看实际数值。","热不热先不猜，照着上报来看。","耗材点名啦，来看看实际用料。","线轴小队报到，信息都在这里。","设备信息来啦，连接情况在这里。","信号小抄到了，来看看上报吧。"},
};
static uint32_t crc32(const uint8_t *p,size_t n) {uint32_t c=~0u;for(size_t i=0;i<n;i++){c^=p[i];for(unsigned b=0;b<8;b++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return ~c;}
bool spirit_external_attach(const void *data,size_t size) {
    if(s_ready)return false; // Never mutate descriptors already retained by LVGL/audio.
    if(!data||size<PACK_BYTES)return false;
    const uint8_t *base=data;uint32_t header[6];memcpy(header,base+8,sizeof(header));
    if(memcmp(base,"PPGST220",8)||header[0]!=1||header[1]!=PACK_BYTES||header[2]!=PACK_CRC||header[3]!=FRAME_COUNT||header[4]!=VOICE_COUNT||header[5]!=0||crc32(base+32,PACK_BYTES-32)!=PACK_CRC)return false;
    for(unsigned i=0;i<FRAME_COUNT;i++) {
        if(s_offsets[i]>PACK_BYTES-6336u)return false;
        s_frames[i]=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_I4,.w=112,.h=112,.stride=56},.data_size=6336,.data=base+s_offsets[i]};
    }

    s_voices[0][0]=(spirit_voice_clip_t){base+456224,77760,12000};
    s_voices[0][1]=(spirit_voice_clip_t){base+495104,77760,12000};
    s_voices[0][2]=(spirit_voice_clip_t){base+533984,61440,12000};
    s_voices[0][3]=(spirit_voice_clip_t){base+564704,64321,12000};
    s_voices[0][4]=(spirit_voice_clip_t){base+596868,66240,12000};
    s_voices[0][5]=(spirit_voice_clip_t){base+629988,44160,12000};
    s_voices[0][6]=(spirit_voice_clip_t){base+652068,40320,12000};
    s_voices[0][7]=(spirit_voice_clip_t){base+672228,54720,12000};
    s_voices[0][8]=(spirit_voice_clip_t){base+699588,45120,12000};
    s_voices[0][9]=(spirit_voice_clip_t){base+722148,42241,12000};
    s_voices[0][10]=(spirit_voice_clip_t){base+743272,49920,12000};
    s_voices[1][0]=(spirit_voice_clip_t){base+768232,51840,12000};
    s_voices[1][1]=(spirit_voice_clip_t){base+794152,75840,12000};
    s_voices[1][2]=(spirit_voice_clip_t){base+832072,50880,12000};
    s_voices[1][3]=(spirit_voice_clip_t){base+857512,60480,12000};
    s_voices[1][4]=(spirit_voice_clip_t){base+887752,53760,12000};
    s_voices[1][5]=(spirit_voice_clip_t){base+914632,38400,12000};
    s_voices[1][6]=(spirit_voice_clip_t){base+933832,33600,12000};
    s_voices[1][7]=(spirit_voice_clip_t){base+950632,37440,12000};
    s_voices[1][8]=(spirit_voice_clip_t){base+969352,48960,12000};
    s_voices[1][9]=(spirit_voice_clip_t){base+993832,39360,12000};
    s_voices[1][10]=(spirit_voice_clip_t){base+1013512,32640,12000};
    s_ready=true;return true;
}
bool spirit_external_ready(void){return s_ready;}
size_t spirit_external_pack_bytes(void){return PACK_BYTES;}
const spirit_device_clip_t *spirit_external_clip(unsigned role,unsigned family,unsigned action) {
    if(!s_ready||role<4||role>=6)return NULL;
    static const uint8_t action_map[8]={0,1,2,2,2,3,0,0};
    return &s_clips[role-4][family<3?family:2][action_map[action<8?action:0]];
}
const spirit_voice_clip_t *spirit_external_voice(unsigned role,unsigned kind) {
    if(!s_ready||role<4||role>=6||kind>=11||!s_voices[role-4][kind].samples)return NULL;
    return &s_voices[role-4][kind];
}
const char *spirit_external_voice_text(unsigned role,unsigned kind) {
    return role>=4&&role<6&&kind<11?s_text[role-4][kind]:NULL;
}
