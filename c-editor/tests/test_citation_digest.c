#include "citation_digest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
static size_t cases;
static void hash_is(const CeCitationDigest *digest,const char *expected) {
    char actual[65],again[65]; unsigned char binary[32]; CeCitationDigest before;
    memcpy(&before,digest,sizeof(before));
    assert(ce_citation_digest_final_hex(digest,actual,error,sizeof(error)));
    if(strcmp(actual,expected)) fprintf(stderr,"SHA256 expected %s actual %s\n",expected,actual);
    assert(!strcmp(actual,expected));
    assert(ce_citation_digest_final_hex(digest,again,error,sizeof(error)) && !strcmp(actual,again));
    assert(ce_citation_digest_final(digest,binary,error,sizeof(error)) && !memcmp(digest,&before,sizeof(before)));
    const char *hex="0123456789abcdef";
    for(size_t i=0;i<32;++i) assert(actual[i*2]==hex[binary[i]>>4] && actual[i*2+1]==hex[binary[i]&15U]);
}
static void vector(const void *data,size_t size,const char *expected) {
    CeCitationDigest digest; ce_citation_digest_init(&digest);
    assert(ce_citation_digest_update(&digest,data,size,error,sizeof(error))); hash_is(&digest,expected);
    const size_t chunks[]={1,7,55,56,63,64,65};
    for(size_t c=0;c<sizeof(chunks)/sizeof(*chunks);++c) {
        ce_citation_digest_init(&digest);
        for(size_t at=0;at<size;) {
            size_t count=size-at; if(count>chunks[c]) count=chunks[c];
            assert(ce_citation_digest_update(&digest,(const unsigned char *)data+at,count,error,sizeof(error))); at+=count;
        }
        assert(ce_citation_digest_update(&digest,NULL,0,error,sizeof(error))); hash_is(&digest,expected);
    }
    ++cases;
}
static unsigned nibble(char c) { return c>='0' && c<='9'?(unsigned)(c-'0'):(unsigned)(c-'a'+10); }
static void hex_vector(const char *message,const char *expected) {
    size_t size=strlen(message)/2; unsigned char *bytes=malloc(size?size:1); assert(bytes);
    for(size_t i=0;i<size;++i) bytes[i]=(unsigned char)((nibble(message[i*2])<<4)|nibble(message[i*2+1]));
    vector(bytes,size,expected); free(bytes);
}
static void test_nist(void) {
    /* NIST NSRL's SHA-256 examples, independently checked from the primary page:
     * https://www.nist.gov/itl/ai/ai-standards-and-guidelines-group/nsrl-test-data */
    vector("abc",3,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *long_text="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    vector(long_text,strlen(long_text),"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    char a[1000]; memset(a,'a',sizeof(a)); CeCitationDigest digest; ce_citation_digest_init(&digest);
    for(size_t i=0;i<1000;++i) assert(ce_citation_digest_update(&digest,a,sizeof(a),error,sizeof(error)));
    hash_is(&digest,"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"); ++cases;
    /* NIST CAVS 11.0 BYTE-oriented SHA256ShortMsg/LongMsg mathematical vectors.
     * https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Algorithm-Validation-Program/documents/shs/shabytetestvectors.zip
     * Retrieved archive SHA256: 929ef80b7b3418aca026643f6f248815913b60e01741a44bba9e118067f4c9b8.
     * Bit lengths 0, 8, 448, 504, 512 and 1304 cover binary and padding boundaries. */
    vector(NULL,0,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    hex_vector("d3","28969cdfa74a12c82f3bad960b0b000aca2ac329deea5c2328ebc6f2ba9802c1");
    hex_vector("2d52447d1244d2ebc28650e7b05654bad35b3a68eedc7f8515306b496d75f3e73385dd1b002625024b81a02f2fd6dffb6e6d561cb7d0bd7a",
        "cfb88d6faf2de3a69d36195acec2e255e2af2b7d933997f348e09f6ce5758360");
    hex_vector("e2f76e97606a872e317439f1a03fcd92e632e5bd4e7cbc4e97f1afc19a16fde92d77cbe546416b51640cddb92af996534dfd81edb17c4424cf1ac4d75aceeb",
        "18041bd4665083001fba8c5411d2d748e8abbfdcdfd9218cb02b68a78e7d4c23");
    hex_vector("5a86b737eaea8ee976a0a24da63e7ed7eefad18a101c1211e2b3650c5187c2a8a650547208251f6d4237e661c7bf4c77f335390394c37fa1a9f9be836ac28509",
        "42e61e174fbb3897d6dd6cef3dd2802fe67b331953b06114a65c772859dfc1aa");
    hex_vector("451101250ec6f26652249d59dc974b7361d571a8101cdfd36aba3b5854d3ae086b5fdd4597721b66e3c0dc5d8c606d9657d0e323283a5217d1f53f2f284f57b85c8a61ac8924711f895c5ed90ef17745ed2d728abd22a5f7a13479a462d71b56c19a74a40b655c58edfe0a188ad2cf46cbf30524f65d423c837dd1ff2bf462ac4198007345bb44dbb7b1c861298cdf61982a833afc728fae1eda2f87aa2c9480858bec",
        "3c593aa539fdcdae516cdf2f15000f6634185c88f505b39775fb9ab137a10aa2");
    ce_citation_digest_init(&digest); assert(ce_citation_digest_update(&digest,"a",1,error,sizeof(error)));
    char first[65]; assert(ce_citation_digest_final_hex(&digest,first,error,sizeof(error)));
    assert(ce_citation_digest_update(&digest,"bc",2,error,sizeof(error)));
    hash_is(&digest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); ++cases;
}
static void field(CeCitationDigest *digest,const char *text) {
    assert(ce_citation_digest_field(digest,text,strlen(text),error,sizeof(error)));
}
static void test_fields(void) {
    CeCitationDigest digest; ce_citation_digest_init(&digest);
    field(&digest,"notelab-citation-v1"); field(&digest,"中文🙂"); field(&digest,""); field(&digest,"x");
    /* Independent Python hashlib over struct.pack('>Q', UTF-8 byte length)
     * plus each exact field. Fixed vectors are independent of this encoder. */
    hash_is(&digest,"0b6f41b41bcc0392095e5892349befc86cc452cbb30a8ccc4a297cd7de90d06a");
    char large[300]; memset(large,'a',sizeof(large)); ce_citation_digest_init(&digest);
    assert(ce_citation_digest_field(&digest,large,sizeof(large),error,sizeof(error)));
    hash_is(&digest,"dc1a27fc85539ae152d698a74d8f1548edb30c070876f6a1b28ddef11447054f");
    const char *pairs[][2]={{"ab","c"},{"a","bc"},{"","abc"},{"abc",""},{"é","x"},{"é","x"}};
    char hashes[6][65];
    for(size_t i=0;i<6;++i) {
        ce_citation_digest_init(&digest); field(&digest,pairs[i][0]); field(&digest,pairs[i][1]);
        assert(ce_citation_digest_final_hex(&digest,hashes[i],error,sizeof(error)));
        for(size_t j=0;j<i;++j) assert(strcmp(hashes[i],hashes[j]));
    }
    ce_citation_digest_init(&digest); field(&digest,"abc");
    char missing[65]; assert(ce_citation_digest_final_hex(&digest,missing,error,sizeof(error)) && strcmp(missing,hashes[3]));
    CeCitationDigest before; memcpy(&before,&digest,sizeof(before));
    const char *bad[]={"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xc2","\x80","\xf0\x9f"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        assert(!ce_citation_digest_field(&digest,bad[i],strlen(bad[i]),error,sizeof(error)) && error[0]);
        assert(!memcmp(&digest,&before,sizeof(digest)));
    }
    const char nul[]={'a',0,'b'};
    assert(!ce_citation_digest_field(&digest,nul,sizeof(nul),error,sizeof(error)) && !memcmp(&digest,&before,sizeof(digest)));
    assert(ce_citation_digest_field(&digest,NULL,0,error,sizeof(error))); cases+=3;
}
static void test_errors_and_overflow(void) {
    CeCitationDigest digest={0},before; unsigned char output[32]; char hex[65];
    memset(output,0x5a,sizeof(output)); memset(hex,'x',sizeof(hex));
    assert(!ce_citation_digest_update(&digest,"x",1,error,sizeof(error)) && error[0]);
    assert(!ce_citation_digest_final(&digest,output,error,sizeof(error)) && output[0]==0x5a && output[31]==0x5a);
    assert(!ce_citation_digest_final_hex(&digest,hex,error,sizeof(error)) && hex[0]=='x' && hex[64]=='x');
    ce_citation_digest_init(NULL); ce_citation_digest_init(&digest); memcpy(&before,&digest,sizeof(before));
    assert(!ce_citation_digest_update(NULL,"x",1,NULL,0));
    assert(!ce_citation_digest_update(&digest,NULL,1,error,sizeof(error)) && !memcmp(&digest,&before,sizeof(digest)));
    assert(!ce_citation_digest_field(&digest,NULL,1,error,sizeof(error)) && !memcmp(&digest,&before,sizeof(digest)));
    assert(!ce_citation_digest_final(&digest,NULL,NULL,0)); assert(!ce_citation_digest_final_hex(&digest,NULL,NULL,0));
    /* Synthetic counters test unreachable-in-practice length boundaries without
     * reading enormous buffers. The internal block contents need not represent
     * an actual long message: only rejection/state preservation is tested. */
    digest._bytes=CE_CITATION_DIGEST_MAX_BYTES; digest._used=(size_t)(digest._bytes&63); memcpy(&before,&digest,sizeof(before));
    assert(!ce_citation_digest_update(&digest,"x",1,error,sizeof(error)) && error[0] && !memcmp(&digest,&before,sizeof(digest)));
    assert(!ce_citation_digest_field(&digest,NULL,0,error,sizeof(error)) && !memcmp(&digest,&before,sizeof(digest)));
    digest._bytes=CE_CITATION_DIGEST_MAX_BYTES-7; digest._used=(size_t)(digest._bytes&63); memcpy(&before,&digest,sizeof(before));
    assert(!ce_citation_digest_field(&digest,"",0,error,sizeof(error)) && !memcmp(&digest,&before,sizeof(digest)));
    digest._bytes=CE_CITATION_DIGEST_MAX_BYTES-8; digest._used=(size_t)(digest._bytes&63);
    assert(ce_citation_digest_field(&digest,"",0,error,sizeof(error)) && digest._bytes==CE_CITATION_DIGEST_MAX_BYTES);
    if(SIZE_MAX>CE_CITATION_DIGEST_MAX_BYTES) {
        ce_citation_digest_init(&digest); memcpy(&before,&digest,sizeof(before));
        assert(!ce_citation_digest_update(&digest,"x",SIZE_MAX,error,sizeof(error)) && !memcmp(&digest,&before,sizeof(digest)));
        assert(!ce_citation_digest_field(&digest,"x",SIZE_MAX,error,sizeof(error)) && !memcmp(&digest,&before,sizeof(digest)));
    }
    ++cases;
}
int main(void) {
    test_nist(); test_fields(); test_errors_and_overflow(); printf("Pure C SHA256 passed (%zu cases)\n",cases); return 0;
}
