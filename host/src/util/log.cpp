#include "util/log.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace twin {

void Log(const char* fmt, ...) {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    tm local{};
    localtime_s(&local, &t);

    char head[64];
    std::snprintf(head, sizeof(head), "[%02d:%02d:%02d] ",
                  local.tm_hour, local.tm_min, local.tm_sec);

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::fprintf(stderr, "%s%s\n", head, msg);
}

}  // namespace twin
