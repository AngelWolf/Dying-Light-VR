// Standalone unit test for src/src/setup/setup_payload.h (Md5Hex, VerifyPayload) - NOT part of any DLL build.
// docs/superpowers/specs/2026-10-17-installer-design.md 8.3: a missing or corrupt embedded file fails BEFORE anything is
// written to the game folder, naming it. Exit code = number of failures.
#include "setup_payload.h"

#include <cstdio>
#include <map>

using namespace dlvr::setup;

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

class MapPayload : public Payload {
public:
    std::map<std::string, std::string> files;
    bool Read(const std::string& rel, std::string* bytes, std::string* why) override
    {
        const auto it = files.find(rel);
        if (it == files.end()) {
            *why = "not embedded";
            return false;
        }
        *bytes = it->second;
        return true;
    }
    bool Manifest(std::string* text, std::string*) override
    {
        *text = "";
        return true;
    }
};

static void md5_known_values()
{
    CHECK(Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
}
static void payload_missing_resource_fails_first()
{
    MapPayload p;
    p.files["a.dll"] = "aaa";
    p.files["c.dll"] = "ccc";
    const std::vector<ManifestEntry> m = {{Md5Hex("aaa"), "a.dll", false},
                                          {Md5Hex("bbb"), "b.dll", false},
                                          {"00000000000000000000000000000000", "c.dll", false}};
    std::string bad;
    CHECK(!VerifyPayload(p, m, &bad));
    CHECK(bad.find("b.dll") != std::string::npos && bad.find("not embedded") != std::string::npos);
}
static void payload_wrong_md5_named()
{
    MapPayload p;
    p.files["a.dll"] = "aaa";
    p.files["c.dll"] = "ccc";
    const std::vector<ManifestEntry> m = {{Md5Hex("aaa"), "a.dll", false},
                                          {"00000000000000000000000000000000", "c.dll", false}};
    std::string bad;
    CHECK(!VerifyPayload(p, m, &bad));
    CHECK(bad.find("c.dll") != std::string::npos && bad.find("md5") != std::string::npos);
}
static void payload_complete_ok()
{
    MapPayload p;
    p.files["a.dll"] = "aaa";
    p.files["DW/b.pak"] = "bbb";
    std::string bad = "untouched";
    CHECK(VerifyPayload(p, {{Md5Hex("aaa"), "a.dll", false}, {Md5Hex("bbb"), "DW/b.pak", false}}, &bad));
    CHECK(bad == "untouched");
}

int main()
{
    md5_known_values();
    payload_missing_resource_fails_first();
    payload_wrong_md5_named();
    payload_complete_ok();
    std::printf("setup_payload_test: %d failure(s)\n", g_fail);
    return g_fail;
}
