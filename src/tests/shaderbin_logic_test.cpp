// Standalone unit test for src/src/common/shaderbin_logic.h - NOT part of the DLL build.
// Shader precompile (docs/superpowers/specs/2026-10-21-shader-precompile-design.md). Exit code = failures.
#include "shaderbin_logic.h"
#include "../bootstrap/shaderbin.gen.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

using namespace dlvr::shaderbin;

static void fnv_vectors()
{
    CHECK(Fnv1a64("", 0) == 0xcbf29ce484222325ull);
    CHECK(Fnv1a64("a", 1) == 0xaf63dc4c8601ec8cull);
}
static void nul_separators_count()
{
    CHECK(MakeKey("a", "b", "c", nullptr, "x", 1, nullptr) != MakeKey("ab", "", "c", nullptr, "x", 1, nullptr));
}
static void hash_extra()
{
    CHECK(MakeKey("a", "b", "c", nullptr, "x", 1, nullptr) != MakeKey("a", "b", "c", nullptr, "x", 1, ""));
    CHECK(MakeKey("a", "b", "c", nullptr, "x", 1, "inc1") != MakeKey("a", "b", "c", nullptr, "x", 1, "inc2"));
}
static void defines()
{
    const Define ab[] = {{"A", "1"}, {"B", "2"}, {nullptr, nullptr}};
    const Define ba[] = {{"B", "2"}, {"A", "1"}, {nullptr, nullptr}};
    const Define none[] = {{nullptr, nullptr}};
    const Define nullv[] = {{"A", nullptr}, {nullptr, nullptr}};
    const Define emptyv[] = {{"A", ""}, {nullptr, nullptr}};
    CHECK(MakeKey("a", "b", "c", ab, "x", 1, nullptr) != MakeKey("a", "b", "c", ba, "x", 1, nullptr));
    CHECK(MakeKey("a", "b", "c", nullptr, "x", 1, nullptr) == MakeKey("a", "b", "c", none, "x", 1, nullptr));
    CHECK(MakeKey("a", "b", "c", ab, "x", 1, nullptr) != MakeKey("a", "b", "c", none, "x", 1, nullptr));
    CHECK(MakeKey("a", "b", "c", nullv, "x", 1, nullptr) == MakeKey("a", "b", "c", emptyv, "x", 1, nullptr));
}
static void src_len_honoured()
{
    CHECK(MakeKey("a", "b", "c", nullptr, "xy", 1, nullptr) == MakeKey("a", "b", "c", nullptr, "x", 1, nullptr));
    CHECK(MakeKey("a", "b", "c", nullptr, "xy", 2, nullptr) != MakeKey("a", "b", "c", nullptr, "x", 1, nullptr));
}
static void null_source_name_is_empty()
{
    CHECK(MakeKey(nullptr, "b", "c", nullptr, "x", 1, nullptr) == MakeKey("", "b", "c", nullptr, "x", 1, nullptr));
}
static void find()
{
    static const unsigned char code[] = {1, 2, 3};
    const Entry t[] = {{10, code, 3, "s", "e1"}, {20, code, 3, "s", "e2"}};
    CHECK(Find(t, 2, 20) == &t[1]);
    CHECK(Find(t, 2, 10) == &t[0]);
    CHECK(Find(t, 2, 30) == nullptr);
    CHECK(Find(t, 0, 10) == nullptr);
}
static void define_terminator_is_null_name()
{
    const Define a[] = {{"A", "1"}, {nullptr, "x"}};
    const Define b[] = {{"A", "1"}, {nullptr, nullptr}};
    CHECK(MakeKey("a", "b", "c", a, "x", 1, nullptr) == MakeKey("a", "b", "c", b, "x", 1, nullptr));
}

// Materialise with a fake blob factory.
struct FakeBlob { unsigned char buf[8]; bool released; };
struct Fake { long rc; bool giveBlob; bool giveData; FakeBlob blob; int calls; size_t gotSize; };
static Fake* g_fake;
static long fake_make(size_t n, void** b, void** d)
{
    ++g_fake->calls;
    g_fake->gotSize = n;
    *b = g_fake->giveBlob ? &g_fake->blob : nullptr;
    *d = g_fake->giveData ? g_fake->blob.buf : nullptr;
    return g_fake->rc;
}
static void fake_release(void* b) { static_cast<FakeBlob*>(b)->released = true; }

static void materialise()
{
    static const unsigned char code[3] = {7, 8, 9};
    const Entry e = {1, code, 3, "s", "e"};
    int sentinel = 0;

    { // success: bytes copied exactly, error blob nulled, blob handed out
        Fake f = {0, true, true, {{0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE}, false}, 0, 0};
        g_fake = &f;
        void* blob = &sentinel; void* err = &sentinel; long rc = -1;
        CHECK(Materialise(e, fake_make, fake_release, &blob, &err, &rc));
        CHECK(blob == &f.blob && err == nullptr && rc == 0 && f.gotSize == 3);
        CHECK(f.blob.buf[0] == 7 && f.blob.buf[1] == 8 && f.blob.buf[2] == 9 && f.blob.buf[3] == 0xEE);
        CHECK(!f.blob.released);
    }
    { // factory fails (no blob): fall through, nothing handed out, error blob untouched
        Fake f = {static_cast<long>(0x8007000E), false, false, {{0}, false}, 0, 0};
        g_fake = &f;
        void* blob = &sentinel; void* err = &sentinel; long rc = 0;
        CHECK(!Materialise(e, fake_make, fake_release, &blob, &err, &rc));
        CHECK(blob == nullptr && err == &sentinel && rc == static_cast<long>(0x8007000E));
    }
    { // factory fails but made a blob: released, nothing handed out
        Fake f = {static_cast<long>(0x8007000E), true, true, {{0}, false}, 0, 0};
        g_fake = &f;
        void* blob = &sentinel; void* err = nullptr; long rc = 0;
        CHECK(!Materialise(e, fake_make, fake_release, &blob, &err, &rc));
        CHECK(blob == nullptr && f.blob.released && rc < 0);
    }
    { // success rc but NULL blob: never S_OK + null
        Fake f = {0, false, false, {{0}, false}, 0, 0};
        g_fake = &f;
        void* blob = &sentinel; void* err = &sentinel; long rc = 0;
        CHECK(!Materialise(e, fake_make, fake_release, &blob, &err, &rc));
        CHECK(blob == nullptr && rc < 0 && err == &sentinel);
    }
    { // success rc, blob but null data: released, fall through
        Fake f = {0, true, false, {{0}, false}, 0, 0};
        g_fake = &f;
        void* blob = &sentinel; void* err = nullptr; long rc = 0;
        CHECK(!Materialise(e, fake_make, fake_release, &blob, &err, &rc));
        CHECK(blob == nullptr && f.blob.released && rc < 0);
    }
}

static void self_test_key()
{
    CHECK(MakeKey("lensmask", "VSTri1", "vs_5_0", nullptr, "float4 x;", 9, nullptr) == gen::kSelfTestKey);
}

int main()
{
    fnv_vectors();
    nul_separators_count();
    hash_extra();
    defines();
    src_len_honoured();
    null_source_name_is_empty();
    find();
    define_terminator_is_null_name();
    materialise();
    self_test_key();
    std::printf("%d failures\n", g_fail);
    return g_fail;
}
