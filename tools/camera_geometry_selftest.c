// Optional on-device tests exercise the actual C implementation, not a mirror.
#include <assert.h>
#include <stddef.h>
#include "camera_geometry.h"
#include "esp_log.h"

void camera_geometry_selftest(void) {
    const int cases[][7] = {
        {1920,1080,240,135,240,135,3}, {1280,720,240,135,240,135,2},
        {1280,960,240,135,180,135,2}, {640,480,240,135,180,135,1},
        {480,640,240,135,101,135,2}, {1920,1200,240,135,216,135,3},
        {1024,768,240,135,180,135,2}, {160,120,240,135,160,120,0},
        {2048,1080,240,135,240,126,3}, {1920,1080,320,180,320,180,2},
    };
    camera_geometry_t g;
    for (size_t i=0; i<sizeof(cases)/sizeof(cases[0]); i++) {
        const int *c=cases[i];
        assert(camera_geometry_fit(c[0],c[1],c[2],c[3],&g));
        assert(g.width==c[4] && g.height==c[5] && g.scale==c[6]);
        assert(g.x==(c[2]-g.width)/2 && g.y==(c[3]-g.height)/2);
    }
    assert(!camera_geometry_fit(0,1080,240,135,&g));
    assert(!camera_geometry_fit(1920,0,240,135,&g));
    assert(!camera_geometry_fit(9000,1080,240,135,&g));
    assert(!camera_geometry_fit(1920,1080,0,135,&g));
    const int dimensions[]={1,2,7,8,16,19,37,127,240,320,641,1080,1920,8192};
    unsigned tested=0;
    for (size_t i=0;i<sizeof(dimensions)/sizeof(dimensions[0]);i++) {
        for (size_t j=0;j<sizeof(dimensions)/sizeof(dimensions[0]);j++) {
            assert(camera_geometry_fit(dimensions[i],dimensions[j],240,135,&g));
            assert(g.x>=0 && g.y>=0 && g.width>0 && g.height>0);
            assert(g.x+g.width<=240 && g.y+g.height<=135);
            for (int x=0;x<g.width;x++) {
                int source=x*g.decoded_w/g.width;
                assert(camera_geometry_edge(source,g.width,g.decoded_w)<=x);
                assert(camera_geometry_edge(source+1,g.width,g.decoded_w)>x);
            }
            for (int y=0;y<g.height;y++) {
                int source=y*g.decoded_h/g.height;
                assert(camera_geometry_edge(source,g.height,g.decoded_h)<=y);
                assert(camera_geometry_edge(source+1,g.height,g.decoded_h)>y);
            }
            tested++;
        }
    }
    ESP_LOGI("camera_test", "geometry passed: 10 layout cases, 196 pixel mappings, invalid sizes");
    assert(tested==196);
}
