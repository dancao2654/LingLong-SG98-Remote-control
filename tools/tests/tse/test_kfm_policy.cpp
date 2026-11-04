#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "TSE/kfm_commissioning_policy.h"
int main() {
    uint8_t f[8] = {1,6,0,0,0,0,0,0};
    for(unsigned reg=0;reg<65536;++reg) {
        f[2]=reg>>8;f[3]=reg;
        for(unsigned value : {0U,1U,2U,17U,100U,150U,500U,501U,799U,800U,801U,1999U,2000U,2001U,63535U,63536U,63537U,1000U,64735U,64736U,64737U,65035U,65036U,65037U,65435U,65436U,65437U,65535U}) {
            f[4]=value>>8;f[5]=value;
            bool expected = (reg==24 && value==0) || (reg==0x98 && value==0) || (reg==0x10F && value==0) || (reg==0x110 && value==2);
#if (defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE) || (defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE)
            if(reg==24)expected=false;
#endif
#if defined(SG98_TSE_KFM_RUN_CANDIDATE) && SG98_TSE_KFM_RUN_CANDIDATE
            if(reg==24)expected=false;
            if(reg==0x99&&value==0)expected=true;
            const bool trusted=expected||(reg==0x10F&&value==1)||(reg==0x99&&static_cast<int16_t>(value)>=-kfm_commissioning::maxRpm&&static_cast<int16_t>(value)<=kfm_commissioning::maxRpm);
            assert(kfm_commissioning::frameAllowed(f,8,8)==trusted);
#else
            assert(kfm_commissioning::frameAllowed(f,8,8)==expected);
#endif
            assert(kfm_commissioning::frameAllowed(f,8)==expected);
#if defined(SG98_TSE_KFM_PA25_PULSE) && SG98_TSE_KFM_PA25_PULSE
            assert(kfm_commissioning::frameAllowed(f,8,4)==(expected||(reg==0x10F&&value==1)||(reg==0x99&&(value==0||value==65436))));
#else
            assert(kfm_commissioning::frameAllowed(f,8,4)==expected);
#endif
#if defined(SG98_TSE_KFM_PA25_PROBE) && SG98_TSE_KFM_PA25_PROBE
            assert(kfm_commissioning::frameAllowed(f,8,2)==(expected||(reg==0x99&&value<=1)));
            assert(kfm_commissioning::frameAllowed(f,8,3)==(expected||(reg==0x99&&value<=1)));
#else
            assert(kfm_commissioning::frameAllowed(f,8,2)==expected);
#endif
#if defined(SG98_TSE_KFM_ZERO_PROBE) && SG98_TSE_KFM_ZERO_PROBE
            assert(kfm_commissioning::frameAllowed(f,8,true)==(expected || (reg==0x10F && value==1)));
#else
            assert(kfm_commissioning::frameAllowed(f,8,true)==expected);
#endif
        }
    }
    assert(!kfm_commissioning::frameAllowed(nullptr,0));
    f[0]=0;assert(!kfm_commissioning::frameAllowed(f,8));f[0]=1;
    f[1]=16;assert(!kfm_commissioning::frameAllowed(f,8));
    f[1]=3;f[2]=0x10;f[3]=0;f[4]=0;f[5]=10;
    assert(kfm_commissioning::frameAllowed(f,8));
    f[5]=0;assert(!kfm_commissioning::frameAllowed(f,8));
    f[5]=17;assert(!kfm_commissioning::frameAllowed(f,8));
    std::puts("KFM transport policy tests passed");
}
