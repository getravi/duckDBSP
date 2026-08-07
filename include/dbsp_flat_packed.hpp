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
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <sys/mman.h>
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
//
// Two storage modes behind one read interface (dir_at/bucket_at/
// arena_data/…): BUILD mode owns std::vectors (folds append here), and
// MAPPED mode points the same views into an mmap'd v2 sidecar file —
// the ~arena-sized RAM class (12GB at 144M) becomes reclaimable page
// cache. Mapped instances are read-only; mutations land in the caller's
// overlay maps exactly as post-restore mutations always have. Move-only
// (owns an fd + mapping).
struct FlatPackedIndex {
  std::vector<uint8_t> arena;
  std::vector<DirEnt> dir; // sorted by key bytes
  std::vector<BucketEnt> buckets;

  FlatPackedIndex() = default;
  FlatPackedIndex(const FlatPackedIndex &) = delete;
  FlatPackedIndex &operator=(const FlatPackedIndex &) = delete;
  FlatPackedIndex(FlatPackedIndex &&o) noexcept { move_from(o); }
  FlatPackedIndex &operator=(FlatPackedIndex &&o) noexcept {
    if (this != &o) {
      unmap();
      move_from(o);
    }
    return *this;
  }
  ~FlatPackedIndex() { unmap(); }

  bool mapped() const { return map_base_ != nullptr; }
  uint64_t dir_size() const { return mapped() ? mdir_n_ : dir.size(); }
  uint64_t buckets_size() const {
    return mapped() ? mbuk_n_ : buckets.size();
  }
  uint64_t arena_size() const { return mapped() ? mare_n_ : arena.size(); }
  const DirEnt &dir_at(uint64_t i) const {
    return mapped() ? mdir_[i] : dir[i];
  }
  const BucketEnt &bucket_at(uint64_t i) const {
    return mapped() ? mbuk_[i] : buckets[i];
  }
  const uint8_t *arena_data() const {
    return mapped() ? mare_ : arena.data();
  }

  bool empty() const { return dir_size() == 0; }

  void clear() {
    unmap();
    arena.clear();
    arena.shrink_to_fit();
    dir.clear();
    dir.shrink_to_fit();
    buckets.clear();
    buckets.shrink_to_fit();
  }

  // RAM actually owned: a mapped index reports its pages as reclaimable
  // (they are file-backed, evictable page cache — not process-resident
  // state in the sense the accounting tracks).
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

  // Adopt a v2 sidecar by mapping it: views point at the file's aligned
  // dir/buckets/arena sections. Offsets are the caller's (it just parsed
  // the header). False leaves this object untouched.
  bool map_file(const std::string &path, uint64_t dir_off, uint64_t ndir,
                uint64_t buk_off, uint64_t nbuk, uint64_t are_off,
                uint64_t nare) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      return false;
    }
    const uint64_t need = are_off + nare;
    void *base = ::mmap(nullptr, static_cast<size_t>(need), PROT_READ,
                        MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) {
      ::close(fd);
      return false;
    }
    clear();
    map_fd_ = fd;
    map_base_ = base;
    map_len_ = static_cast<size_t>(need);
    const auto *b = static_cast<const uint8_t *>(base);
    mdir_ = reinterpret_cast<const DirEnt *>(b + dir_off);
    mdir_n_ = ndir;
    mbuk_ = reinterpret_cast<const BucketEnt *>(b + buk_off);
    mbuk_n_ = nbuk;
    mare_ = b + are_off;
    mare_n_ = nare;
    return true;
  }

  const DirEnt *find(const std::string &kb) const {
    const uint64_t n = dir_size();
    if (n == 0) {
      return nullptr;
    }
    const uint8_t *base = arena_data();
    const auto *key = reinterpret_cast<const uint8_t *>(kb.data());
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
      const uint64_t mid = lo + (hi - lo) / 2;
      const DirEnt &e = dir_at(mid);
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

private:
  void *map_base_ = nullptr;
  size_t map_len_ = 0;
  int map_fd_ = -1;
  const DirEnt *mdir_ = nullptr;
  uint64_t mdir_n_ = 0;
  const BucketEnt *mbuk_ = nullptr;
  uint64_t mbuk_n_ = 0;
  const uint8_t *mare_ = nullptr;
  uint64_t mare_n_ = 0;

  void unmap() {
    if (map_base_ != nullptr) {
      ::munmap(map_base_, map_len_);
      map_base_ = nullptr;
    }
    if (map_fd_ >= 0) {
      ::close(map_fd_);
      map_fd_ = -1;
    }
    mdir_ = nullptr;
    mdir_n_ = 0;
    mbuk_ = nullptr;
    mbuk_n_ = 0;
    mare_ = nullptr;
    mare_n_ = 0;
  }

  void move_from(FlatPackedIndex &o) {
    arena = std::move(o.arena);
    dir = std::move(o.dir);
    buckets = std::move(o.buckets);
    map_base_ = o.map_base_;
    map_len_ = o.map_len_;
    map_fd_ = o.map_fd_;
    mdir_ = o.mdir_;
    mdir_n_ = o.mdir_n_;
    mbuk_ = o.mbuk_;
    mbuk_n_ = o.mbuk_n_;
    mare_ = o.mare_;
    mare_n_ = o.mare_n_;
    o.map_base_ = nullptr;
    o.map_len_ = 0;
    o.map_fd_ = -1;
    o.mdir_ = nullptr;
    o.mdir_n_ = 0;
    o.mbuk_ = nullptr;
    o.mbuk_n_ = 0;
    o.mare_ = nullptr;
    o.mare_n_ = 0;
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
// v2: dir/buckets/arena sections start 8-byte ALIGNED so an adopt can
// mmap the file and read the raw structs in place (v1 packed them right
// after the variable-length header — unaligned, copy-load only). v1
// files are rejected: one arrangement backfill on first reopen after
// the upgrade, same precedent as the digest-index v1->v2 bump.
constexpr uint32_t kFlatFileVersion = 2;

inline uint64_t pad8(uint64_t x) { return (x + 7) & ~uint64_t{7}; }

inline bool write_flat_index_file(
    const std::string &path, const std::string &fingerprint,
    int64_t wm_count, const std::string &wm_hash, const FlatPackedIndex &fp) {
  std::FILE *f = std::fopen((path + ".tmp").c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  uint64_t written = 0;
  auto put = [&](const void *p, size_t n) {
    std::fwrite(p, 1, n, f);
    written += n;
  };
  auto pad_to8 = [&]() {
    static const char zeros[8] = {0};
    const uint64_t want = pad8(written);
    if (want > written) {
      put(zeros, static_cast<size_t>(want - written));
    }
  };
  const uint64_t magic = kFlatFileMagic;
  const uint32_t ver = kFlatFileVersion;
  const uint32_t fplen = static_cast<uint32_t>(fingerprint.size());
  const uint32_t hlen = static_cast<uint32_t>(wm_hash.size());
  const uint64_t ndir = fp.dir_size();
  const uint64_t nbuk = fp.buckets_size();
  const uint64_t nare = fp.arena_size();
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
  pad_to8();
  if (ndir) {
    put(&fp.dir_at(0), ndir * sizeof(DirEnt)); // serves owned OR mapped
  }
  pad_to8();
  if (nbuk) {
    put(&fp.bucket_at(0), nbuk * sizeof(BucketEnt));
  }
  pad_to8();
  if (nare) {
    put(fp.arena_data(), nare);
  }
  std::fflush(f);
  ::fsync(fileno(f));
  std::fclose(f);
  std::error_code ec;
  std::filesystem::rename(path + ".tmp", path, ec);
  return !ec;
}

// ---- delta sidecar (delta-append saves) ---------------------------------
// REPLACEMENT buckets for the keys touched since the base file was
// adopted, chained to the base by ITS watermark: a dirty save writes
// O(touched buckets) instead of folding the whole arrangement. An empty
// replacement masks the base bucket (key emptied). The loader adopts the
// base, then converts each replacement into overlay deltas.

constexpr uint64_t kFlatDeltaMagic = 0xDB5BF1A7DE17A5F1ULL;
constexpr uint32_t kFlatDeltaVersion = 1;

using ReplacementBuckets =
    std::vector<std::pair<std::string,
                          std::vector<std::pair<std::string, int64_t>>>>;

inline bool write_flat_delta_file(const std::string &path,
                                  const std::string &fingerprint,
                                  int64_t wm_count, const std::string &wm_hash,
                                  int64_t base_wm_count,
                                  const std::string &base_wm_hash,
                                  const ReplacementBuckets &buckets) {
  std::FILE *f = std::fopen((path + ".tmp").c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  auto put = [&](const void *p, size_t n) { std::fwrite(p, 1, n, f); };
  const uint64_t magic = kFlatDeltaMagic;
  const uint32_t ver = kFlatDeltaVersion;
  const uint32_t fplen = static_cast<uint32_t>(fingerprint.size());
  const uint32_t hlen = static_cast<uint32_t>(wm_hash.size());
  const uint32_t bhlen = static_cast<uint32_t>(base_wm_hash.size());
  const uint64_t nkeys = buckets.size();
  put(&magic, 8);
  put(&ver, 4);
  put(&fplen, 4);
  put(fingerprint.data(), fplen);
  put(&wm_count, 8);
  put(&hlen, 4);
  put(wm_hash.data(), hlen);
  put(&base_wm_count, 8);
  put(&bhlen, 4);
  put(base_wm_hash.data(), bhlen);
  put(&nkeys, 8);
  for (const auto &[kb, rows] : buckets) {
    const uint32_t klen = static_cast<uint32_t>(kb.size());
    const uint64_t nrows = rows.size();
    put(&klen, 4);
    put(kb.data(), klen);
    put(&nrows, 8);
    for (const auto &[rb, w] : rows) {
      const uint32_t rlen = static_cast<uint32_t>(rb.size());
      put(&rlen, 4);
      put(rb.data(), rlen);
      put(&w, 8);
    }
  }
  std::fflush(f);
  ::fsync(fileno(f));
  std::fclose(f);
  std::error_code ec;
  std::filesystem::rename(path + ".tmp", path, ec);
  return !ec;
}

inline bool load_flat_delta_file(const std::string &path,
                                 const std::string &fingerprint,
                                 int64_t wm_count, const std::string &wm_hash,
                                 int64_t &base_wm_count_out,
                                 std::string &base_wm_hash_out,
                                 ReplacementBuckets &out) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  auto get = [&](void *p, size_t n) { return std::fread(p, 1, n, f) == n; };
  uint64_t magic = 0, nkeys = 0;
  uint32_t ver = 0, fplen = 0, hlen = 0, bhlen = 0;
  int64_t count = 0, base_count = 0;
  bool ok = get(&magic, 8) && magic == kFlatDeltaMagic && get(&ver, 4) &&
            ver == kFlatDeltaVersion && get(&fplen, 4);
  std::string fp_in(fplen, '\0');
  ok = ok && (fplen == 0 || get(fp_in.data(), fplen)) && get(&count, 8) &&
       get(&hlen, 4);
  std::string hash(hlen, '\0');
  ok = ok && (hlen == 0 || get(hash.data(), hlen)) && get(&base_count, 8) &&
       get(&bhlen, 4);
  std::string base_hash(bhlen, '\0');
  ok = ok && (bhlen == 0 || get(base_hash.data(), bhlen)) && get(&nkeys, 8);
  if (!ok || fp_in != fingerprint || count != wm_count || hash != wm_hash) {
    std::fclose(f);
    return false;
  }
  ReplacementBuckets loaded;
  loaded.reserve(nkeys);
  for (uint64_t i = 0; ok && i < nkeys; i++) {
    uint32_t klen = 0;
    uint64_t nrows = 0;
    ok = get(&klen, 4);
    std::string kb(klen, '\0');
    ok = ok && (klen == 0 || get(kb.data(), klen)) && get(&nrows, 8);
    std::vector<std::pair<std::string, int64_t>> rows;
    rows.reserve(nrows);
    for (uint64_t r = 0; ok && r < nrows; r++) {
      uint32_t rlen = 0;
      int64_t w = 0;
      ok = get(&rlen, 4);
      std::string rb(rlen, '\0');
      ok = ok && (rlen == 0 || get(rb.data(), rlen)) && get(&w, 8);
      rows.emplace_back(std::move(rb), w);
    }
    loaded.emplace_back(std::move(kb), std::move(rows));
  }
  std::fclose(f);
  if (!ok) {
    return false;
  }
  base_wm_count_out = base_count;
  base_wm_hash_out = std::move(base_hash);
  out = std::move(loaded);
  return true;
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
  const long header_end = ok ? std::ftell(f) : -1;
  std::fclose(f);
  if (!ok || header_end < 0 || fp_in != fingerprint || count != wm_count ||
      hash != wm_hash) {
    return false;
  }
  // v2 section layout: each section starts 8-byte aligned.
  const uint64_t dir_off = pad8(static_cast<uint64_t>(header_end));
  const uint64_t buk_off = pad8(dir_off + ndir * sizeof(DirEnt));
  const uint64_t are_off = pad8(buk_off + nbuk * sizeof(BucketEnt));
  std::error_code ec;
  const uint64_t fsize = std::filesystem::file_size(path, ec);
  if (ec || fsize < are_off + nare) {
    return false; // truncated
  }
  // Adopt by MAPPING: the arena-class bytes stay reclaimable page cache
  // instead of process-resident copies.
  FlatPackedIndex loaded;
  if (!loaded.map_file(path, dir_off, ndir, buk_off, nbuk, are_off, nare)) {
    return false;
  }
  out = std::move(loaded);
  return true;
}

} // namespace flatpacked

} // namespace dbsp_native
