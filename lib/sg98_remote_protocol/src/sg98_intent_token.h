#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

// Idempotency for physical Start/Stop gestures, not an authentication scheme.
// Token = controller boot/cancel epoch (64 bits) + monotonic gesture sequence (32 bits).
namespace sg98_intent {
struct Token { uint64_t boot; uint32_t sequence; };
inline bool parse(const char* text, Token& out) {
    if(!text || std::strlen(text) != 24) return false;
    uint64_t boot=0; uint32_t sequence=0;
    for(unsigned i=0;i<24;++i) {
        const char c=text[i]; unsigned digit;
        if(c>='0'&&c<='9')digit=c-'0';
        else if(c>='a'&&c<='f')digit=c-'a'+10;
        else if(c>='A'&&c<='F')digit=c-'A'+10;
        else return false;
        if(i<16)boot=(boot<<4)|digit;else sequence=(sequence<<4)|digit;
    }
    if(!boot || !sequence) return false;
    out={boot,sequence};return true;
}
inline void format(char (&out)[25],uint64_t boot,uint32_t sequence) {
    std::snprintf(out,sizeof(out),"%016llx%08lx",static_cast<unsigned long long>(boot),static_cast<unsigned long>(sequence));
}
enum class Decision { New, DuplicateAccepted, DuplicateRejected, Invalid, Busy };
class Ledger {
    uint64_t boot_=0; uint32_t next_=1,last_=0; char action_=0;
    bool accepted_=false,busy_=false;
public:
    bool seeded()const{return boot_!=0;}
    void reset(uint64_t epoch){boot_=epoch?epoch:1;next_=1;last_=0;action_=0;accepted_=false;busy_=false;}
    void seed(uint64_t boot){if(!boot_)boot_=boot?boot:1;}
    void offer(char (&out)[25])const {if(!boot_||!next_){out[0]=0;return;}format(out,boot_,next_);}
    Decision begin(const char* text,char action) {
        Token t{};if(!parse(text,t)||t.boot!=boot_||!next_)return Decision::Invalid;
        if(busy_)return Decision::Busy;
        if(t.sequence<next_) {
            if(t.sequence==last_&&action==action_)return accepted_?Decision::DuplicateAccepted:Decision::DuplicateRejected;
            return Decision::Invalid;
        }
        // Skipped sequences are allowed: a physical Stop must survive a lost Start.
        last_=t.sequence;action_=action;accepted_=false;busy_=true;
        next_=t.sequence==std::numeric_limits<uint32_t>::max()?0:t.sequence+1;
        return Decision::New;
    }
    void finish(bool accepted){accepted_=accepted;busy_=false;}
};
class Client {
    uint64_t boot_=0;uint32_t last_=0;
public:
    bool next(const char* advertised,char (&out)[25]) {
        Token t{};if(!parse(advertised,t))return false;
        if(t.boot==boot_) {
            if(last_==std::numeric_limits<uint32_t>::max())return false;
            if(t.sequence<=last_)t.sequence=last_+1;
        }
        format(out,t.boot,t.sequence);boot_=t.boot;last_=t.sequence;return true;
    }
};
}
