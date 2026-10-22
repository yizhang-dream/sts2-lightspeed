// STS2 RNG layer — faithful port of MegaCrit.Sts2.Core.Random (game decompiled source is authority).
//
//   MegaRandom : Xoshiro256** seeded via Splitmix64 (Redzen-derived, see game source header).
//   Sts2Rng    : gameplay wrapper with a call counter; NextInt(n) = (int)(NextDouble()*n).
//   xxhash64   : used by StringHelper.GetDeterministicHashCode (UTF-8 bytes, seed 0) for named
//                stream seeding: Rng(seed, name) => MegaRandom(seed + xxhash64(snake_case_name)).
//
// All arithmetic must match the C# exactly (uint64 wraparound, double truncation toward zero).

#ifndef STS2_RANDOM_H
#define STS2_RANDOM_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace sts2 {

// ---------- XXH64 (public domain algorithm, seed 0 for GetDeterministicHashCode) ----------
namespace xxh64_detail {

constexpr std::uint64_t P1 = 11400714785074694791ULL;
constexpr std::uint64_t P2 = 14029467366897019727ULL;
constexpr std::uint64_t P3 = 1609587929392839161ULL;
constexpr std::uint64_t P4 = 9650029242287828579ULL;
constexpr std::uint64_t P5 = 2870177450012600261ULL;

constexpr std::uint64_t rotl64(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

struct Acc {
    std::uint64_t v[4];
};

constexpr Acc initAcc(std::uint64_t seed) {
    return {{seed + P1 + P2, seed + P2, seed, seed - P1}};
}

constexpr std::uint64_t round(std::uint64_t acc, std::uint64_t input) {
    acc += input * P2;
    acc = rotl64(acc, 31);
    return acc * P1;
}

constexpr std::uint64_t mergeRound(std::uint64_t acc, std::uint64_t val) {
    val = round(0, val);
    acc ^= val;
    return acc * P1 + P4;
}

constexpr std::uint64_t read64(const unsigned char* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
constexpr std::uint32_t read32(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

constexpr std::uint64_t finalize(std::uint64_t h, const unsigned char* p, std::size_t len) {
    // len & 15 bytes remain (0..15); process 8, then 4, then 1
    while (len >= 8) {
        h ^= round(0, read64(p));
        h = rotl64(h, 27) * P1 + P4;
        p += 8; len -= 8;
    }
    if (len >= 4) {
        h ^= static_cast<std::uint64_t>(read32(p)) * P1;
        h = rotl64(h, 23) * P2 + P3;
        p += 4; len -= 4;
    }
    while (len > 0) {
        h ^= (*p) * P5;
        h = rotl64(h, 11) * P1;
        ++p; --len;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

} // namespace xxh64_detail

// XXH64 over raw bytes with explicit seed (StringHelper.GetDeterministicHashCode uses seed 0).
inline std::uint64_t xxhash64(const void* data, std::size_t len, std::uint64_t seed = 0) {
    using namespace xxh64_detail;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    const unsigned char* const end = p + len;

    std::uint64_t h;
    if (len >= 32) {
        const Acc a = initAcc(seed);
        std::uint64_t v0 = a.v[0], v1 = a.v[1], v2 = a.v[2], v3 = a.v[3];
        const unsigned char* const limit = end - 32;
        do {
            v0 = round(v0, read64(p));      p += 8;
            v1 = round(v1, read64(p));      p += 8;
            v2 = round(v2, read64(p));      p += 8;
            v3 = round(v3, read64(p));      p += 8;
        } while (p <= limit);
        h = rotl64(v0, 1) + rotl64(v1, 7) + rotl64(v2, 12) + rotl64(v3, 18);
        h = mergeRound(h, v0);
        h = mergeRound(h, v1);
        h = mergeRound(h, v2);
        h = mergeRound(h, v3);
    } else {
        h = seed + P5;
    }
    h += static_cast<std::uint64_t>(len);
    // Only the bytes the 32-byte loop did not consume are left for finalize; passing the
    // original len here read past the buffer for inputs >= 32 bytes (seed strings).
    return finalize(h, p, static_cast<std::size_t>(end - p));
}

inline std::uint64_t xxhash64(const std::string& s, std::uint64_t seed = 0) {
    return xxhash64(s.data(), s.size(), seed);
}

// ---------- MegaRandom: Xoshiro256** + Splitmix64 seeding ----------
class MegaRandom {
public:
    MegaRandom() { Reinitialise(0); }
    explicit MegaRandom(std::uint64_t seed) { Reinitialise(seed); }

    static std::uint64_t Splitmix64(std::uint64_t& x) {
        std::uint64_t num = (x += 11400714819323198485ULL);
        num = (num ^ (num >> 30)) * 13787848793156543929ULL;
        num = (num ^ (num >> 27)) * 10723151780598845931ULL;
        return num ^ (num >> 31);
    }

    void Reinitialise(std::uint64_t seed) {
        _s0 = Splitmix64(seed);
        _s1 = Splitmix64(seed);
        _s2 = Splitmix64(seed);
        _s3 = Splitmix64(seed);
    }

    std::uint64_t NextULong() { return nextULongInner(); }
    std::uint32_t NextUInt() { return static_cast<std::uint32_t>(nextULongInner()); }

    // [0, maxValue)
    int Next(int maxValue) { return nextInner(maxValue); }
    // [minValue, maxValue)
    int Next(int minValue, int maxValue) {
        const long long n = static_cast<long long>(maxValue) - minValue;
        if (n <= 0x7FFFFFFFLL) {
            return nextInner(static_cast<int>(n)) + minValue;
        }
        return static_cast<int>(nextInnerLL(n) + minValue);
    }

    double NextDouble() {
        return static_cast<double>(nextULongInner() >> 11) * 1.1102230246251565E-16;
    }

    float NextFloat() {
        return static_cast<float>(static_cast<double>(nextULongInner() >> 40) * 5.9604645E-08);
    }

    // state capture (SerializableRng)
    struct State { std::uint64_t s0, s1, s2, s3; };
    State Capture() const { return {_s0, _s1, _s2, _s3}; }
    void Restore(const State& s) { _s0 = s.s0; _s1 = s.s1; _s2 = s.s2; _s3 = s.s3; }

private:
    std::uint64_t nextULongInner() {
        // Xoshiro256** core, exact game-source sequence:
        //   s2 ^= s0; s3 ^= s1; s1 ^= s2; s0 ^= s3; s2 ^= t; s3 = rotl(s3, 45);
        const std::uint64_t result = xxh64_detail::rotl64(_s1 * 5, 7) * 9;
        const std::uint64_t t = _s1 << 17;
        _s2 ^= _s0;
        _s3 ^= _s1;
        _s1 ^= _s2;
        _s0 ^= _s3;
        _s2 ^= t;
        _s3 = xxh64_detail::rotl64(_s3, 45);
        return result;
    }

    int nextInner(int maxValue) {
        return static_cast<int>(NextDouble() * static_cast<double>(maxValue));
    }
    long long nextInnerLL(long long maxValue) {
        return static_cast<long long>(NextDouble() * static_cast<double>(maxValue));
    }

    std::uint64_t _s0, _s1, _s2, _s3;
};

// ---------- Sts2Rng: the gameplay-facing Rng (counter + MegaRandom) ----------
class Sts2Rng {
public:
    Sts2Rng() = default;
    explicit Sts2Rng(std::uint64_t seed) : _random(seed) {}
    // Rng(seed, name): seed + xxhash64(name)
    Sts2Rng(std::uint64_t seed, const std::string& name)
        : _random(seed + xxhash64(name)) {}

    bool NextBool() { ++_counter; return _random.Next(2) == 0; }

    // [0, maxExclusive)
    int NextInt(int maxExclusive = 0x7FFFFFFF) { ++_counter; return _random.Next(maxExclusive); }
    // [minInclusive, maxExclusive)
    int NextInt(int minInclusive, int maxExclusive) {
        ++_counter;
        return _random.Next(minInclusive, maxExclusive);
    }

    std::uint32_t NextUnsignedInt(std::uint32_t minInclusive, std::uint32_t maxExclusive) {
        ++_counter;
        const double d = _random.NextDouble();
        const double span = static_cast<double>(maxExclusive) - minInclusive;
        return minInclusive + static_cast<std::uint32_t>(d * span);
    }

    std::uint64_t NextUnsignedLong() { ++_counter; return _random.NextULong(); }

    float NextFloat(float min = 0.0f, float max = 1.0f) {
        ++_counter;
        return static_cast<float>(_random.NextDouble() * static_cast<double>(max - min) + static_cast<double>(min));
    }

    double NextDouble() { ++_counter; return _random.NextDouble(); }

    // Rng.NextGaussianInt(mean, stdDev, min, max): Box-Muller with Math.Round (banker's
    // rounding), resampled until inside [min, max]. Map generation consumes this.
    int NextGaussianInt(int mean, int stdDev, int min, int max) {
        int v = min;
        do {
            const double u1 = 1.0 - NextDouble();
            const double u2 = 1.0 - NextDouble();
            const double radius = std::sqrt(-2.0 * std::log(u1));
            const double theta = 3.14159265358979323846 * 2.0 * u2;
            const double raw = static_cast<double>(mean) +
                static_cast<double>(stdDev) * (radius * std::sin(theta));
            v = static_cast<int>(std::nearbyint(raw));
        } while (v < min || v > max);
        return v;
    }

    int Counter() const { return _counter; }

    // SerializableRng round trip (counter + 4 state words)
    struct Serializable { int counter; MegaRandom::State state; };
    Serializable CaptureState() const { return {_counter, _random.Capture()}; }
    void LoadFromSerializable(const Serializable& s) {
        _counter = s.counter;
        _random.Restore(s.state);
    }

private:
    int _counter = 0;
    MegaRandom _random;
};

} // namespace sts2

#endif // STS2_RANDOM_H
