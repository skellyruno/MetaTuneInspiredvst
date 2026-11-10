#pragma once
// Minimal test harness: no dependency, failures counted and reported, nonzero exit on failure.
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace th
{
inline int failures = 0;
inline std::vector<std::pair<const char*, std::function<void()>>>& registry()
{
    static std::vector<std::pair<const char*, std::function<void()>>> r;
    return r;
}
struct Reg { Reg (const char* n, std::function<void()> f) { registry().emplace_back (n, std::move (f)); } };
inline int runAll()
{
    for (auto& [name, fn] : registry())
    {
        const int before = failures;
        fn();
        std::printf ("%s %s\n", failures == before ? "PASS" : "FAIL", name);
    }
    std::printf ("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
} // namespace th

#define TH_CAT2(a, b) a##b
#define TH_CAT(a, b) TH_CAT2(a, b)
#define TEST(name) static void name(); static th::Reg TH_CAT(reg_, name) (#name, name); static void name()
#define CHECK(c) do { if (! (c)) { ++th::failures; std::printf ("  %s:%d CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, tol) do { const double a_ = (a), b_ = (b); if (! (std::abs (a_ - b_) <= (tol))) { ++th::failures; \
    std::printf ("  %s:%d CHECK_NEAR(%s, %s) %.6g vs %.6g (tol %g)\n", __FILE__, __LINE__, #a, #b, a_, b_, double (tol)); } } while (0)
