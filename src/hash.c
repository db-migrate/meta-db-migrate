/**
 * SHA-256, for the one thing the core hashes: a migration's file, which node
 * keeps in the lock row (`h`) to tell whether an interrupted migration was
 * changed before it is resumed. Here so the core needs no OpenSSL.
 *
 * FIPS 180-4, written plainly; checked against the standard's test vectors
 * in test/options.sh.
 */
#include <db_migrate_driver.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint32_t h[8];
  uint64_t length;
  unsigned char block[64];
  size_t used;
} sha256_t;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

static void compress(sha256_t *self, const unsigned char *block) {

  uint32_t w[64];
  uint32_t a, b, c, d, e, f, g, h;

  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
           (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];

  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  a = self->h[0], b = self->h[1], c = self->h[2], d = self->h[3];
  e = self->h[4], f = self->h[5], g = self->h[6], h = self->h[7];

  for (int i = 0; i < 64; ++i) {
    uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t choose = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + choose + K[i] + w[i];
    uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + majority;

    h = g, g = f, f = e, e = d + t1;
    d = c, c = b, b = a, a = t1 + t2;
  }

  self->h[0] += a, self->h[1] += b, self->h[2] += c, self->h[3] += d;
  self->h[4] += e, self->h[5] += f, self->h[6] += g, self->h[7] += h;
}

static void begin(sha256_t *self) {

  static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                      0xa54ff53a, 0x510e527f, 0x9b05688c,
                                      0x1f83d9ab, 0x5be0cd19};

  memcpy(self->h, initial, sizeof initial);
  self->length = 0;
  self->used = 0;
}

static void feed(sha256_t *self, const unsigned char *data, size_t length) {

  self->length += length;

  while (length > 0) {

    size_t take = 64 - self->used < length ? 64 - self->used : length;

    memcpy(self->block + self->used, data, take);
    self->used += take;
    data += take;
    length -= take;

    if (self->used == 64) {
      compress(self, self->block);
      self->used = 0;
    }
  }
}

static void finish(sha256_t *self, char hex[65]) {

  uint64_t bits = self->length * 8;
  unsigned char pad = 0x80;
  unsigned char zero = 0;
  unsigned char tail[8];

  feed(self, &pad, 1);

  while (self->used != 56)
    feed(self, &zero, 1);

  for (int i = 0; i < 8; ++i)
    tail[i] = (unsigned char)(bits >> (56 - 8 * i));

  feed(self, tail, 8);

  for (int i = 0; i < 8; ++i)
    snprintf(hex + i * 8, 9, "%08x", self->h[i]);
}

void dbmSha256(const void *data, size_t length, char hex[65]) {

  sha256_t self;

  begin(&self);
  feed(&self, data, length);
  finish(&self, hex);
}

bool dbmSha256File(const char *path, char hex[65]) {

  FILE *file = fopen(path, "rb");
  unsigned char buffer[8192];
  size_t got;
  sha256_t self;

  if (file == NULL)
    return false;

  begin(&self);

  while ((got = fread(buffer, 1, sizeof buffer, file)) > 0)
    feed(&self, buffer, got);

  bool ok = !ferror(file);

  fclose(file);
  finish(&self, hex);
  return ok;
}
