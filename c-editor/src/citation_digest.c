#include "citation_digest.h"
#include <stdio.h>
#include <string.h>

/* SHA-256, FIPS 180-4 sections 4.1.2, 4.2.2, 5 and 6.2.
 * https://csrc.nist.gov/pubs/fips/180-4/upd1/final
 * Independent implementation; constants are the standard's mathematical data. */
#define DIGEST_INITIALIZED UINT32_C(0x53483236)
static const uint32_t rounds[64] = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};
static uint32_t rotate(uint32_t value, unsigned amount) { return (value >> amount) | (value << (32 - amount)); }
static void compress(CeCitationDigest *digest, const unsigned char block[64]) {
    uint32_t words[64];
    for(size_t i=0;i<16;++i) words[i]=((uint32_t)block[4*i]<<24)|((uint32_t)block[4*i+1]<<16)|
        ((uint32_t)block[4*i+2]<<8)|(uint32_t)block[4*i+3];
    for(size_t i=16;i<64;++i) {
        uint32_t a=words[i-15],b=words[i-2];
        words[i]=words[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+words[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
    }
    uint32_t a=digest->_state[0],b=digest->_state[1],c=digest->_state[2],d=digest->_state[3];
    uint32_t e=digest->_state[4],f=digest->_state[5],g=digest->_state[6],h=digest->_state[7];
    for(size_t i=0;i<64;++i) {
        uint32_t first=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+rounds[i]+words[i];
        uint32_t second=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+first;d=c;c=b;b=a;a=first+second;
    }
    digest->_state[0]+=a;digest->_state[1]+=b;digest->_state[2]+=c;digest->_state[3]+=d;
    digest->_state[4]+=e;digest->_state[5]+=f;digest->_state[6]+=g;digest->_state[7]+=h;
}
static bool fail(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"Citation digest: %s",message);
    return false;
}
static bool valid(const CeCitationDigest *digest) {
    return digest && digest->_initialized==DIGEST_INITIALIZED && digest->_bytes<=CE_CITATION_DIGEST_MAX_BYTES &&
        digest->_used<64 && digest->_used==(size_t)(digest->_bytes&63);
}
void ce_citation_digest_init(CeCitationDigest *digest) {
    if(!digest) return;
    memset(digest,0,sizeof(*digest));
    static const uint32_t initial[8]={0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    memcpy(digest->_state,initial,sizeof(initial)); digest->_initialized=DIGEST_INITIALIZED;
}
bool ce_citation_digest_update(CeCitationDigest *digest,const void *data,size_t size,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!valid(digest) || (!data && size)) return fail(error,capacity,"invalid state or input");
    if(size>CE_CITATION_DIGEST_MAX_BYTES-digest->_bytes) return fail(error,capacity,"message length overflow");
    const unsigned char *bytes=data;
    digest->_bytes+=(uint64_t)size;
    if(digest->_used && size) {
        size_t amount=64-digest->_used; if(amount>size) amount=size;
        memcpy(digest->_block+digest->_used,bytes,amount); digest->_used+=amount; bytes+=amount; size-=amount;
        if(digest->_used==64) { compress(digest,digest->_block); digest->_used=0; }
    }
    while(size>=64) { compress(digest,bytes); bytes+=64; size-=64; }
    if(size) { memcpy(digest->_block,bytes,size); digest->_used=size; }
    return true;
}
static bool utf8_valid(const char *text,size_t size) {
    const unsigned char *p=(const unsigned char *)text;
    for(size_t at=0;at<size;) {
        unsigned char c=p[at++];
        if(!c) return false;
        if(c<0x80) continue;
        uint32_t value,minimum; unsigned count;
        if(c>=0xc2 && c<=0xdf) { value=c&31U;minimum=0x80;count=1; }
        else if(c>=0xe0 && c<=0xef) { value=c&15U;minimum=0x800;count=2; }
        else if(c>=0xf0 && c<=0xf4) { value=c&7U;minimum=0x10000;count=3; }
        else return false;
        if(count>size-at) return false;
        for(unsigned i=0;i<count;++i) {
            c=p[at++]; if((c&0xc0)!=0x80) return false; value=(value<<6)|(c&63U);
        }
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return false;
    }
    return true;
}
bool ce_citation_digest_field(CeCitationDigest *digest,const char *utf8,size_t size,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!valid(digest) || (!utf8 && size)) return fail(error,capacity,"invalid state or field");
    uint64_t available=CE_CITATION_DIGEST_MAX_BYTES-digest->_bytes;
    if(available<8 || size>available-8)
        return fail(error,capacity,"field length overflow");
    if(!utf8_valid(utf8,size)) return fail(error,capacity,"field is not NUL-free UTF-8");
    unsigned char prefix[8]; uint64_t length=(uint64_t)size;
    for(size_t i=0;i<8;++i) prefix[7-i]=(unsigned char)(length>>(i*8));
    /* Both updates have been prevalidated; no partial prefix can be committed. */
    return ce_citation_digest_update(digest,prefix,sizeof(prefix),error,capacity) &&
        ce_citation_digest_update(digest,utf8,size,error,capacity);
}
bool ce_citation_digest_final(const CeCitationDigest *digest,unsigned char out[32],char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!valid(digest) || !out) return fail(error,capacity,"invalid finalization arguments");
    CeCitationDigest final=*digest;
    final._block[final._used++]=0x80;
    if(final._used>56) { memset(final._block+final._used,0,64-final._used); compress(&final,final._block); final._used=0; }
    memset(final._block+final._used,0,56-final._used);
    uint64_t bits=final._bytes*8;
    for(size_t i=0;i<8;++i) final._block[63-i]=(unsigned char)(bits>>(i*8));
    compress(&final,final._block);
    for(size_t i=0;i<8;++i) for(size_t byte=0;byte<4;++byte) out[i*4+byte]=(unsigned char)(final._state[i]>>(24-8*byte));
    return true;
}
bool ce_citation_digest_final_hex(const CeCitationDigest *digest,char out[65],char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!out) return fail(error,capacity,"invalid hex output");
    unsigned char bytes[32]; if(!ce_citation_digest_final(digest,bytes,error,capacity)) return false;
    static const char hex[]="0123456789abcdef";
    for(size_t i=0;i<32;++i) { out[2*i]=hex[bytes[i]>>4]; out[2*i+1]=hex[bytes[i]&15U]; }
    out[64]=0; return true;
}
