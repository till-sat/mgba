/* SPDX-License-Identifier: MPL-2.0 */
#include "../tools/gba_next_spike_guest_copy.h"
#include <array>
#include <vector>
#include <cassert>
#include <cstdio>
struct Memory {
    std::array<std::vector<char>,4> pages;
    bool hole=false;
    Memory() {for(auto& p:pages)p.resize(4096+64,char(0xa5));}
    char* addr_to_mem(uint32_t a) {
        if(a<0xa0000000 || a>=0xa0004000)return nullptr;
        unsigned offset=a-0xa0000000,page=offset/4096;
        if(hole && page==1)return nullptr;
        return pages[page].data()+(offset&4095);
    }
};
int main() {
    Memory m;std::vector<char> flat(16384),out(16392,char(0x5a));
    for(unsigned i=0;i<flat.size();++i) {
        flat[i]=char((i*37+(i>>12)*11+0x31)&255);
        m.pages[i/4096][i&4095]=flat[i];
    }
    unsigned cases=0;unsigned long comparisons=0;
    for(unsigned at:{0,1,3,4,4092,4093,4095,4096,4097,8191})
    for(unsigned bytes:{0,1,3,4,63,127,4095,4096,4097,8192}) {
        assert(at+bytes<=flat.size());std::fill(out.begin(),out.end(),char(0x5a));
        gbn_copy_guest_pages(&m,out.data()+4,0xa0000000+at,bytes);
        assert(!memcmp(out.data()+4,flat.data()+at,bytes));
        for(unsigned i=0;i<4;++i)assert(out[i]==char(0x5a) && out[4+bytes+i]==char(0x5a));
        comparisons+=bytes+8;++cases;
    }
    // Safe reproduction: old contiguous read lands in allocated page padding.
    char bad[8];memcpy(bad,m.addr_to_mem(0xa0000ffc),8);
    assert(memcmp(bad,flat.data()+4092,8));
    char good[8];gbn_copy_guest_pages(&m,good,0xa0000ffc,8);
    assert(!memcmp(good,flat.data()+4092,8));
    bool failed=false;m.hole=true;
    try {gbn_copy_guest_pages(&m,out.data(),0xa0000ffe,4);}catch(const std::runtime_error&){failed=true;}
    assert(failed);failed=false;
    try {gbn_copy_guest_pages(&m,out.data(),0xfffffff0,32);}catch(const std::runtime_error&){failed=true;}
    assert(failed);failed=false;
    try {gbn_copy_guest_pages(&m,out.data(),0xa0004000,1);}catch(const std::runtime_error&){failed=true;}
    assert(failed);
    printf("PASS sparse page reader: %u ranges, %lu byte/guard comparisons; old cross-page read rejected, holes and RV32 wrap rejected\n",cases,comparisons);
}
