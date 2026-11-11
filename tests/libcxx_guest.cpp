#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <pthread.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>
#include <algorithm>

static unsigned initialized;
static void require(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "FAIL guest libc++: %s\n", message); std::abort(); }
}
static const std::string &shared_string()
{
    static std::string text = [] { usleep(10000); initialized++; return std::string(256, 'x'); }();
    return text;
}
static void *worker(void *)
{
    require(shared_string().size() == 256 && shared_string().back() == 'x', "thread-safe local static string");
    return (void *)42;
}
int main(int argc, char **argv)
{
    require(argc == 2, "file argument");
    std::string text(128, 'x'); text.append(" shader"); text.erase(0, 128);
    require(text == " shader" && text.find('s') == 1, "string growth, erase and find");
    std::vector<int> values = {42, -1, 17, 0}; std::sort(values.begin(), values.end());
    require(values.front() == -1 && values.back() == 42, "vector and sort");
    std::map<std::string, int> ordered; ordered["answer"] = 42;
    std::unordered_map<std::string, int> hashed; hashed["answer"] = ordered.at("answer");
    require(hashed.at("answer") == 42, "ordered and hashed maps");
    std::ostringstream stream; stream << "answer=" << hashed.at("answer") << " pi=" << 2.5;
    require(stream.str() == "answer=42 pi=2.5", "locale and formatted stream");
    std::out_of_range exception("diagnostic");
    require(std::strcmp(exception.what(), "diagnostic") == 0, "exception object without unwinding");
    { std::ofstream file(argv[1]); file << "shader source\n"; require(file.good(), "write file stream"); }
    { std::ifstream file(argv[1]); std::getline(file, text); require(text == "shader source", "read file stream"); }
    require(std::chrono::system_clock::now().time_since_epoch().count() > 0, "clock");
    pthread_t first, second; void *a, *b;
    require(pthread_create(&first, nullptr, worker, nullptr) == 0 && pthread_create(&second, nullptr, worker, nullptr) == 0, "start static initializer threads");
    require(pthread_join(first, &a) == 0 && pthread_join(second, &b) == 0 && a == (void *)42 && b == (void *)42 && initialized == 1,
            "guest C++ guard initializes once and releases waiting threads");
    std::puts("PASS guest libc++ strings, containers, streams, locale, clocks and concurrent guards");
    return 0;
}
