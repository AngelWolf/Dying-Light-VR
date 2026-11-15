// launcher_logs.h + launcher_zip.h - user-name masking and the log zip (spec section 11; plan Review Focus 2).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "launcher_logs.h"
#include "launcher_zip.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace dlvr::launcher;

static std::string U16(const std::string& a)
{
    std::string o;
    for (char c : a) { o.push_back(c); o.push_back('\0'); }
    return o;
}

static size_t CountCI(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool m = true;
        for (size_t k = 0; k < needle.size() && m; ++k) {
            m = std::tolower(static_cast<unsigned char>(hay[i + k])) == std::tolower(static_cast<unsigned char>(needle[k]));
        }
        n += m ? 1 : 0;
    }
    return n;
}

int main()
{
    const auto names = NameForms("Alex Sample", "C:\\Users\\ALEXSA~1", {});
    CHECK(names.size() == 2);
    {   // POSITIVE CONTROL: the name in every form -> none left, every one counted
        const std::string t = "C:\\Users\\Alex Sample\\Documents\r\nc:\\users\\ALEX SAMPLE\\AppData\r\nC:\\Users\\ALEXSA~1\\x\r\n" +
                              U16("Alex Sample") + "\r\n";
        int hits = 0;
        const std::string m = MaskText(t, names, &hits);
        CHECK(hits == 4);
        CHECK(CountCI(m, "alex sample") == 0 && CountCI(m, "alexsa~1") == 0 && CountCI(m, U16("alex sample")) == 0);
        CHECK(m.find("C:\\Users\\<user>\\Documents") != std::string::npos);
    }
    {   // binary: same size, only the name bytes change
        const std::string b = std::string("\x01\x02", 2) + "Alex Sample" + std::string("\x00\xff", 2) + U16("ALEX SAMPLE") + "end";
        int hits = 0;
        const std::string m = MaskBinary(b, names, &hits);
        CHECK(hits == 2);
        CHECK(m.size() == b.size());
        CHECK(m.substr(0, 2) == b.substr(0, 2) && m.substr(m.size() - 3) == "end");
        CHECK(CountCI(m, "alex sample") == 0 && CountCI(m, U16("alex sample")) == 0);
    }
    {   // NEGATIVE: no name -> 0 hits, text unchanged
        int hits = -1;
        CHECK(MaskText("nothing here\r\n", names, &hits) == "nothing here\r\n" && hits == 0);
    }
    {   // an extra name (an Epic profile name) is masked too; a too-short name is never used (it would mask everything)
        const auto n2 = NameForms("Jo", "", {"FoxPlayer99"});
        int hits = 0;
        const std::string m = MaskText("user FoxPlayer99 joined; Jo said", n2, &hits);
        CHECK(m.find("FoxPlayer99") == std::string::npos && m.find("Jo said") != std::string::npos && hits == 1);
    }
    {   // newest n by time
        const std::vector<std::pair<std::string, unsigned long long>> f = {{"a", 5}, {"b", 9}, {"c", 1}, {"d", 7}};
        const auto p = PickNewest(f, 2);
        CHECK(p.size() == 2 && p[0] == "b" && p[1] == "d");
        CHECK(PickNewest(f, 10).size() == 4);
    }
    {   // a store-only zip that Windows' own tar lists, names with '/'
        const std::string z = ZipStore({{"logs/dlvr_ovr2xr.log", "hello"}, {"crash/crash_1.log", std::string(70000, 'x')}});
        const char* path = "launcher_logs_test.zip";
        { std::ofstream o(path, std::ios::binary); o.write(z.data(), static_cast<std::streamsize>(z.size())); }
        const int rc = std::system("C:\\Windows\\System32\\tar.exe -tf launcher_logs_test.zip > launcher_logs_test.txt");
        CHECK(rc == 0);
        std::ifstream l("launcher_logs_test.txt");
        std::string all((std::istreambuf_iterator<char>(l)), std::istreambuf_iterator<char>());
        CHECK(all.find("logs/dlvr_ovr2xr.log") != std::string::npos && all.find("crash/crash_1.log") != std::string::npos);
        const int rc2 = std::system("C:\\Windows\\System32\\tar.exe -xOf launcher_logs_test.zip logs/dlvr_ovr2xr.log > launcher_logs_x.txt");
        std::ifstream x("launcher_logs_x.txt", std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(x)), std::istreambuf_iterator<char>());
        CHECK(rc2 == 0 && got == "hello");
        x.close();
        l.close();
        std::remove(path);
        std::remove("launcher_logs_test.txt");
        std::remove("launcher_logs_x.txt");
        CHECK(Crc32("123456789") == 0xCBF43926u);   // the standard check value
    }
    std::printf("launcher_logs_test: %d failure(s)\n", g_fail);
    return g_fail;
}
