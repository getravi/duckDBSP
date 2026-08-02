// dbsp_flat_packed.hpp — contiguous restore-side layout for packed join
// state (bounded-RAM tier 2, inc 2).
//
// Restoring a packed join index used to rebuild
// unordered_map<string, vector<pair<string,int64>>> — at 36M rows that is
// tens of seconds of string allocations and hash inserts, ~all of the
// first-edit-after-reopen cost. These structures decode the SAME
// checkpoint blob into one contiguous byte arena plus a directory sorted
// by key bytes: build is append + one contiguous sort, probes are binary
// searches. They are IMMUTABLE after build — post-restore mutations land
// in the node's ordinary packed maps, which act as a DELTA overlay
// (weights sum across layers; the next checkpoint save folds both layers
// back into one blob stream).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace dbsp_native {

namespace flatpacked {

struct DirEnt {
  uint64_t key_off;
  uint32_t key_len;
  uint64_t bucket_off; // index into buckets[]
  uint32_t bucket_n;
};

struct BucketEnt {
  uint64_t row_off;
  uint32_t row_len;
  int64_t weight;
};

struct WeightEnt {
  uint64_t row_off;
  uint32_t row_len;
  int64_t weight;
};

inline int cmp_bytes(const uint8_t *a, size_t alen, const uint8_t *b,
                     size_t blen) {
  const int c = std::memcmp(a, b, std::min(alen, blen));
  if (c != 0) {
    return c;
  }
  return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

// Immutable key -> bucket index over one shared byte arena.
struct FlatPackedIndex {
  std::vector<uint8_t> arena;
  std::vector<DirEnt> dir; // sorted by key bytes
  std::vector<BucketEnt> buckets;

  bool empty() const { return dir.empty(); }

  void clear() {
    arena.clear();
    arena.shrink_to_fit();
    dir.clear();
    dir.shrink_to_fit();
    buckets.clear();
    buckets.shrink_to_fit();
  }

  size_t resident_bytes() const {
    return arena.capacity() + dir.capacity() * sizeof(DirEnt) +
           buckets.capacity() * sizeof(BucketEnt);
  }

  uint64_t append_bytes(const std::string &b) {
    const uint64_t off = arena.size();
    arena.insert(arena.end(), b.begin(), b.end());
    return off;
  }

  void finish_build() {
    const uint8_t *base = arena.data();
    std::sort(dir.begin(), dir.end(),
              [base](const DirEnt &a, const DirEnt &b) {
                return cmp_bytes(base + a.key_off, a.key_len,
                                 base + b.key_off, b.key_len) < 0;
              });
  }

  const DirEnt *find(const std::string &kb) const {
    if (dir.empty()) {
      return nullptr;
    }
    const uint8_t *base = arena.data();
    const auto *key = reinterpret_cast<const uint8_t *>(kb.data());
    size_t lo = 0, hi = dir.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const DirEnt &e = dir[mid];
      const int c = cmp_bytes(base + e.key_off, e.key_len, key, kb.size());
      if (c == 0) {
        return &e;
      }
      if (c < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return nullptr;
  }
};

// Immutable row-bytes -> weight index over its own arena.
struct FlatPackedWeights {
  std::vector<uint8_t> arena;
  std::vector<WeightEnt> dir; // sorted by row bytes

  bool empty() const { return dir.empty(); }

  void clear() {
    arena.clear();
    arena.shrink_to_fit();
    dir.clear();
    dir.shrink_to_fit();
  }

  size_t resident_bytes() const {
    return arena.capacity() + dir.capacity() * sizeof(WeightEnt);
  }

  void finish_build() {
    const uint8_t *base = arena.data();
    std::sort(dir.begin(), dir.end(),
              [base](const WeightEnt &a, const WeightEnt &b) {
                return cmp_bytes(base + a.row_off, a.row_len,
                                 base + b.row_off, b.row_len) < 0;
              });
  }

  int64_t find(const std::string &rb) const {
    if (dir.empty()) {
      return 0;
    }
    const uint8_t *base = arena.data();
    const auto *key = reinterpret_cast<const uint8_t *>(rb.data());
    size_t lo = 0, hi = dir.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const WeightEnt &e = dir[mid];
      const int c = cmp_bytes(base + e.row_off, e.row_len, key, rb.size());
      if (c == 0) {
        return e.weight;
      }
      if (c < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return 0;
  }
};

// ---- durable sidecar (shared arrangements, recovery inc 3) --------------
// One file per (arrangement fingerprint): header + dir + buckets + arena,
// entries sorted by key bytes so a load is three bulk reads and ZERO
// build work. Trusted only when the stored fingerprint AND the source
// table's watermark match; anything else is rejected and the arrangement
// backfills from the baseline as before.

constexpr uint64_t kFlatFileMagic = 0xDB5BF1A7A44A96E5ULL;
constexpr uint32_t kFlatFileVersion = 1;

inline bool write_flat_index_file(
    const std::string &path, const std::string &fingerprint,
    int64_t wm_count, const std::string &wm_hash, const FlatPackedIndex &fp) {
  std::FILE *f = std::fopen((path + ".tmp").c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  auto put = [&](const void *p, size_t n) { std::fwrite(p, 1, n, f); };
  const uint64_t magic = kFlatFileMagic;
  const uint32_t ver = kFlatFileVersion;
  const uint32_t fplen = static_cast<uint32_t>(fingerprint.size());
  const uint32_t hlen = static_cast<uint32_t>(wm_hash.size());
  const uint64_t ndir = fp.dir.size();
  const uint64_t nbuk = fp.buckets.size();
  const uint64_t nare = fp.arena.size();
  put(&magic, 8);
  put(&ver, 4);
  put(&fplen, 4);
  put(fingerprint.data(), fplen);
  put(&wm_count, 8);
  put(&hlen, 4);
  put(wm_hash.data(), hlen);
  put(&ndir, 8);
  put(&nbuk, 8);
  put(&nare, 8);
  put(fp.dir.data(), ndir * sizeof(DirEnt));
  put(fp.buckets.data(), nbuk * sizeof(BucketEnt));
  put(fp.arena.data(), nare);
  std::fflush(f);
  ::fsync(fileno(f));
  std::fclose(f);
  std::error_code ec;
  std::filesystem::rename(path + ".tmp", path, ec);
  return !ec;
}

inline bool load_flat_index_file(const std::string &path,
                                 const std::string &fingerprint,
                                 int64_t wm_count, const std::string &wm_hash,
                                 FlatPackedIndex &out) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  auto get = [&](void *p, size_t n) { return std::fread(p, 1, n, f) == n; };
  uint64_t magic = 0, ndir = 0, nbuk = 0, nare = 0;
  uint32_t ver = 0, fplen = 0, hlen = 0;
  int64_t count = 0;
  bool ok = get(&magic, 8) && magic == kFlatFileMagic && get(&ver, 4) &&
            ver == kFlatFileVersion && get(&fplen, 4);
  std::string fp_in(fplen, '\0');
  ok = ok && (fplen == 0 || get(fp_in.data(), fplen)) && get(&count, 8) &&
       get(&hlen, 4);
  std::string hash(hlen, '\0');
  ok = ok && (hlen == 0 || get(hash.data(), hlen)) && get(&ndir, 8) &&
       get(&nbuk, 8) && get(&nare, 8);
  if (!ok || fp_in != fingerprint || count != wm_count || hash != wm_hash) {
    std::fclose(f);
    return false;
  }
  FlatPackedIndex loaded;
  loaded.dir.resize(ndir);
  loaded.buckets.resize(nbuk);
  loaded.arena.resize(nare);
  ok = (ndir == 0 || get(loaded.dir.data(), ndir * sizeof(DirEnt))) &&
       (nbuk == 0 || get(loaded.buckets.data(), nbuk * sizeof(BucketEnt))) &&
       (nare == 0 || get(loaded.arena.data(), nare));
  std::fclose(f);
  if (!ok) {
    return false;
  }
  out = std::move(loaded);
  return true;
}

} // namespace flatpacked

} // namespace dbsp_native
