// xex_patch.cpp - XEX2 delta patch () application tool.
// Synthesizes a complete flat executable image by applying a delta patch
// (default.xexp) onto the base XEX (default.xex), following the xenia
// implementation (src/xenia/cpu/xex_module.cc ApplyPatch / ReadImage*).
//
// Build (VS2017 BuildTools):
//   vcvars64.bat && cl /nologo /O2 /EHsc /I src src\mspack\lzxd.c src\main.cpp /Fe:xex_patch.exe
//
// Usage:
//   xex_patch.exe <base.xex> <patch.xexp> <out.flat>
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------- mspack (defines mspack_system/mspack_file/lzxd_stream) ----
#include "mspack/mspack.h"
#include "mspack/lzx.h"

// ---------------- xenia_log (required by lzxd.c) ----------------
extern "C" void xenia_log(const char* fmt, ...) {
  // Debug logging from mspack; keep quiet unless needed.
  (void)fmt;
}

// ---------------- TinySHA1 ----------------
#include "crypto/TinySHA1.hpp"

// ---------------- Rijndael AES (public domain, used by xenia) ----------------
#include "crypto/rijndael-alg-fst.c"

// ---------------- Big-endian helpers (XEX is big-endian) ----------------
static uint16_t rd_be16(const uint8_t* p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t rd_be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}
static uint64_t rd_be64(const uint8_t* p) {
  return ((uint64_t)rd_be32(p) << 32) | rd_be32(p + 4);
}

// ---------------- AES-128-CBC decrypt with IV=0 (xenia aes_decrypt_buffer) --
static void aes_128_cbc_decrypt(const uint8_t* key, const uint8_t* input,
                                size_t input_size, uint8_t* output) {
  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t ivec[16] = {0};
  int32_t Nr = rijndaelKeySetupDec(rk, key, 128);
  const uint8_t* ct = input;
  uint8_t* pt = output;
  for (size_t n = 0; n < input_size; n += 16, ct += 16, pt += 16) {
    rijndaelDecrypt(rk, Nr, ct, pt);
    for (size_t i = 0; i < 16; i++) {
      pt[i] ^= ivec[i];
      ivec[i] = ct[i];
    }
  }
}

// ---------------- SHA1 wrapper ----------------
static void sha1_calc(const uint8_t* data, size_t len, uint8_t digest[20]) {
  sha1::SHA1 s;
  s.reset();
  s.processBytes(data, len);
  s.finalize(digest);
}

// ---------------- mspack memory file (mirror of xenia lzx.cc) ----------------
typedef struct mspack_memory_file_t {
  mspack_system sys;
  void* buffer;
  off_t buffer_size;
  off_t offset;
} mspack_memory_file;

static mspack_memory_file* mspack_memory_open(mspack_system* sys, void* buffer,
                                              const size_t buffer_size) {
  if (buffer_size >= INT_MAX) return NULL;
  mspack_memory_file* memfile =
      (mspack_memory_file*)calloc(1, sizeof(mspack_memory_file));
  if (!memfile) return NULL;
  memfile->buffer = buffer;
  memfile->buffer_size = (off_t)buffer_size;
  memfile->offset = 0;
  return memfile;
}

static void mspack_memory_close(mspack_memory_file* file) { free(file); }

static int mspack_memory_read(mspack_file* file, void* buffer, int chars) {
  mspack_memory_file* memfile = (mspack_memory_file*)file;
  const off_t remaining = memfile->buffer_size - memfile->offset;
  const off_t total = remaining < (off_t)chars ? remaining : (off_t)chars;
  memcpy(buffer, (uint8_t*)memfile->buffer + memfile->offset, (size_t)total);
  memfile->offset += total;
  return (int)total;
}

static int mspack_memory_write(mspack_file* file, void* buffer, int chars) {
  mspack_memory_file* memfile = (mspack_memory_file*)file;
  const off_t remaining = memfile->buffer_size - memfile->offset;
  const off_t total = remaining < (off_t)chars ? remaining : (off_t)chars;
  memcpy((uint8_t*)memfile->buffer + memfile->offset, buffer, (size_t)total);
  memfile->offset += total;
  return (int)total;
}

static void* mspack_memory_alloc(mspack_system* sys, size_t chars) {
  return calloc(chars, 1);
}
static void mspack_memory_free(void* ptr) { free(ptr); }
static void mspack_memory_copy(void* src, void* dest, size_t chars) {
  memcpy(dest, src, chars);
}

static mspack_system* mspack_memory_sys_create() {
  mspack_system* sys = (mspack_system*)calloc(1, sizeof(mspack_system));
  if (!sys) return NULL;
  sys->read = mspack_memory_read;
  sys->write = mspack_memory_write;
  sys->alloc = mspack_memory_alloc;
  sys->free = mspack_memory_free;
  sys->copy = mspack_memory_copy;
  return sys;
}
static void mspack_memory_sys_destroy(mspack_system* sys) { free(sys); }

static int bit_scan_forward(uint32_t value, uint32_t* out_index) {
  if (!value) return 0;
  uint32_t idx = 0;
  while (!(value & 1)) {
    value >>= 1;
    idx++;
  }
  *out_index = idx;
  return 1;
}

// LZX decompress with optional reference window (mirror of xenia lzx.cc).
static int lzx_decompress(const void* lzx_data, size_t lzx_len, void* dest,
                          size_t dest_len, uint32_t window_size,
                          void* window_data, size_t window_data_len) {
  int result_code = 1;
  uint32_t window_bits = 0;
  if (!bit_scan_forward(window_size, &window_bits)) return result_code;

  mspack_system* sys = mspack_memory_sys_create();
  mspack_memory_file* lzxsrc = mspack_memory_open(sys, (void*)lzx_data, lzx_len);
  mspack_memory_file* lzxdst = mspack_memory_open(sys, dest, dest_len);
  lzxd_stream* lzxd = lzxd_init(sys, (mspack_file*)lzxsrc, (mspack_file*)lzxdst,
                                (int)window_bits, 0, 0x8000, (off_t)dest_len, 0);
  if (lzxd) {
    if (window_data) {
      size_t padding_len = window_size - window_data_len;
      memset(&lzxd->window[0], 0, padding_len);
      memcpy(&lzxd->window[padding_len], window_data, window_data_len);
      lzxd->ref_data_size = window_size;
    }
    result_code = lzxd_decompress(lzxd, (off_t)dest_len);
    lzxd_free(lzxd);
    lzxd = NULL;
  }
  if (lzxsrc) { mspack_memory_close(lzxsrc); lzxsrc = NULL; }
  if (lzxdst) { mspack_memory_close(lzxdst); lzxdst = NULL; }
  if (sys) { mspack_memory_sys_destroy(sys); sys = NULL; }
  return result_code;
}

// XEX2 structures (big-endian on disk).
struct xex2_delta_patch {
  uint32_t old_addr;          // be32
  uint32_t new_addr;          // be32
  uint16_t uncompressed_len;  // be16
  uint16_t compressed_len;    // be16
  // uint8_t patch_data[compressed_len];
};

// Apply one delta record stream (mirror of xenia lzxdelta_apply_patch).
static int lzxdelta_apply_patch(uint8_t* patch, size_t patch_len,
                                uint32_t window_size, void* dest) {
  uint8_t* patch_end = patch + patch_len;
  uint8_t* cur_patch = patch;
  while (patch_end > cur_patch) {
    int patch_sz = -4;
    xex2_delta_patch* dp = (xex2_delta_patch*)cur_patch;
    uint32_t old_addr = rd_be32(cur_patch);
    uint32_t new_addr = rd_be32(cur_patch + 4);
    uint16_t uncompressed_len = rd_be16(cur_patch + 8);
    uint16_t compressed_len = rd_be16(cur_patch + 10);
    (void)dp;
    if (compressed_len == 0 && uncompressed_len == 0 && new_addr == 0 &&
        old_addr == 0) {
      break;
    }
    switch (compressed_len) {
      case 0:  // fill with 0
        memset((char*)dest + new_addr, 0, uncompressed_len);
        break;
      case 1:  // copy from old -> new
        memcpy((char*)dest + new_addr, (char*)dest + old_addr,
               uncompressed_len);
        break;
      default: {  // delta
        patch_sz = (int)compressed_len - 4;
        int result = lzx_decompress(cur_patch + 12, compressed_len,
                                    (char*)dest + new_addr, uncompressed_len,
                                    window_size, (char*)dest + old_addr,
                                    uncompressed_len);
        if (result) return result;
        break;
      }
    }
    // Advance over the 12-byte record header and the compressed payload.
    // (xenia advances a 16-byte-aligned struct pointer then adds
    // (compressed_len-4), which equals 12+compressed_len for delta records
    // and 12 for fill/copy records.)
    if (compressed_len <= 1) {
      cur_patch += 12;
    } else {
      cur_patch += 12 + compressed_len;
    }
    (void)patch_sz;
  }
  return 0;
}

// ---------------- XEX2 header parsing ----------------
struct Xex2OptHeader {
  uint32_t key;
  uint32_t value;  // value or offset depending on key low bits
};

struct Xex2File {
  std::vector<uint8_t> data;
  uint32_t header_size = 0;
  uint32_t security_offset = 0;
  uint32_t header_count = 0;
  std::vector<Xex2OptHeader> opts;

  const uint8_t* header() const { return data.data(); }

  bool parse() {
    if (data.size() < 0x18) return false;
    if (memcmp(data.data(), "XEX2", 4) != 0) return false;
    header_size = rd_be32(data.data() + 8);
    security_offset = rd_be32(data.data() + 0x10);
    header_count = rd_be32(data.data() + 0x14);
    if (header_size > data.size()) return false;
    if (security_offset + 0x184 > header_size) return false;
    for (uint32_t i = 0; i < header_count; i++) {
      uint32_t o = 0x18 + i * 8;
      if (o + 8 > header_size) return false;
      Xex2OptHeader oh;
      oh.key = rd_be32(data.data() + o);
      oh.value = rd_be32(data.data() + o + 4);
      opts.push_back(oh);
    }
    return true;
  }

  // Get opt header data pointer; returns nullptr if not found.
  // For keys with low byte 0 -> value itself (offset<0), for 0xFF -> data ptr.
  const uint8_t* get_opt(uint32_t key, uint32_t* out_offset = nullptr) const {
    for (const auto& oh : opts) {
      if (oh.key == key) {
        if ((key & 0xFF) == 0xFF) {
          if (out_offset) *out_offset = oh.value;
          return data.data() + oh.value;
        }
        if (out_offset) *out_offset = oh.value;
        return nullptr;  // value-style header; caller uses out_offset
      }
    }
    return nullptr;
  }
};

// security info field offsets (xex2_security_info)
enum {
  SEC_OFF_IMAGE_SIZE = 0x04,
  SEC_OFF_AES_KEY = 0x150,
  SEC_OFF_IMAGE_FLAGS = 0x10C,
  SEC_OFF_LOAD_ADDRESS = 0x110,
  SEC_OFF_EXPORT_TABLE = 0x160,
  SEC_OFF_PAGE_DESC_COUNT = 0x180,
  SEC_OFF_PAGE_DESCRIPTORS = 0x184,
};

// ---------------- Main ----------------
static bool read_file(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  out->resize((size_t)sz);
  size_t rd = fread(out->data(), 1, (size_t)sz, f);
  fclose(f);
  return rd == (size_t)sz;
}

static bool write_file(const std::string& path, const uint8_t* data,
                       size_t size) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  size_t wr = fwrite(data, 1, size, f);
  fclose(f);
  return wr == size;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr,
            "usage: xex_patch <base.xex> <patch.xexp> <out.flat> "
            "[--dump-base <base.flat>]\n");
    return 1;
  }
  const std::string base_path = argv[1];
  const std::string patch_path = argv[2];
  const std::string out_path = argv[3];
  const bool dump_base = (argc >= 6 && strcmp(argv[4], "--dump-base") == 0);
  const std::string base_out = dump_base ? argv[5] : std::string();

  // xex2 retail key
  static const uint8_t xe_xex2_retail_key[16] = {
      0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
      0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

  // ---- load base ----
  std::vector<uint8_t> base_raw;
  if (!read_file(base_path, &base_raw)) {
    fprintf(stderr, "cannot read base file %s\n", base_path.c_str());
    return 1;
  }
  Xex2File base;
  base.data = base_raw;
  if (!base.parse()) {
    fprintf(stderr, "base XEX parse failed\n");
    return 1;
  }
  // opt file format info
  uint32_t base_ffi_off = 0;
  const uint8_t* base_ffi = base.get_opt(0x3FF, &base_ffi_off);
  if (!base_ffi || base_ffi_off + 0x28 > base.header_size) {
    fprintf(stderr, "base FFI missing\n");
    return 1;
  }
  uint32_t base_ffi_info_size = rd_be32(base_ffi);
  uint16_t base_enc = rd_be16(base_ffi + 4);
  uint16_t base_comp = rd_be16(base_ffi + 6);
  fprintf(stderr, "base: header_size=0x%X enc=%u comp=%u info_size=0x%X\n",
          base.header_size, base_enc, base_comp, base_ffi_info_size);
  if (base_comp != 1) {  // XEX_COMPRESSION_BASIC
    fprintf(stderr, "base compression type %u not BASIC(1); unsupported\n",
            base_comp);
    return 1;
  }

  const uint8_t* base_sec = base.data.data() + base.security_offset;
  uint32_t base_image_size = rd_be32(base_sec + SEC_OFF_IMAGE_SIZE);
  uint8_t base_aes_key[16];
  memcpy(base_aes_key, base_sec + SEC_OFF_AES_KEY, 16);
  uint32_t base_page_desc_count = rd_be32(base_sec + SEC_OFF_PAGE_DESC_COUNT);
  fprintf(stderr, "base: image_size=0x%X page_desc_count=%u aes=", base_image_size,
          base_page_desc_count);
  for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", base_aes_key[i]);
  fprintf(stderr, "\n");

  // compute total image size from page descriptors
  uint64_t base_total_size = 0;
  for (uint32_t i = 0; i < base_page_desc_count; i++) {
    const uint8_t* pd = base_sec + SEC_OFF_PAGE_DESCRIPTORS + i * 0x18;
    uint32_t v = rd_be32(pd);
    uint32_t info = v & 0xF;
    uint32_t page_count = v >> 4;
    (void)info;
    base_total_size += (uint64_t)page_count * 0x10000;
  }
  fprintf(stderr, "base: total_size from page descriptors = 0x%llX\n",
          (unsigned long long)base_total_size);
  if (base_total_size != base_image_size) {
    fprintf(stderr, "warning: page descriptor total != image_size\n");
  }

  // ---- decrypt base payload (ReadImageBasicCompressed) ----
  uint8_t base_session_key[16];
  aes_128_cbc_decrypt(xe_xex2_retail_key, base_aes_key, 16, base_session_key);

  const uint8_t* base_payload = base.data.data() + base.header_size;
  size_t base_payload_len = base.data.size() - base.header_size;

  std::vector<uint8_t> base_image(base_total_size, 0);
  uint8_t* d = base_image.data();
  const uint8_t* p = base_payload;
  // basic blocks: (info_size - 8) / 8
  uint32_t block_count = (base_ffi_info_size - 8) / 8;
  const uint8_t* blocks = base_ffi + 8;
  for (uint32_t n = 0; n < block_count; n++) {
    uint32_t data_size = rd_be32(blocks + n * 8);
    uint32_t zero_size = rd_be32(blocks + n * 8 + 4);
    if ((size_t)(p - base_payload) + data_size > base_payload_len) {
      fprintf(stderr, "base payload out of range in block %u\n", n);
      return 1;
    }
    if ((size_t)(d - base_image.data()) + data_size > base_image.size()) {
      fprintf(stderr, "base image out of range in block %u\n", n);
      return 1;
    }
    if (base_enc == 1) {
      aes_128_cbc_decrypt(base_session_key, p, data_size, d);
    } else {
      memcpy(d, p, data_size);
    }
    p += data_size;
    d += data_size + zero_size;
  }
  // sanity check PE header
  if (memcmp(base_image.data(), "MZ", 2) != 0) {
    fprintf(stderr, "ERROR: base image has no MZ header; bad decrypt?\n");
    return 1;
  }
  fprintf(stderr, "base: image decrypted OK (MZ), size=0x%llX\n",
          (unsigned long long)base_image.size());

  // ---- load patch ----
  std::vector<uint8_t> patch_raw;
  if (!read_file(patch_path, &patch_raw)) {
    fprintf(stderr, "cannot read patch file %s\n", patch_path.c_str());
    return 1;
  }
  Xex2File patch;
  patch.data = patch_raw;
  if (!patch.parse()) {
    fprintf(stderr, "patch XEX parse failed\n");
    return 1;
  }
  uint32_t patch_ffi_off = 0;
  const uint8_t* patch_ffi = patch.get_opt(0x3FF, &patch_ffi_off);
  if (!patch_ffi) {
    fprintf(stderr, "patch FFI missing\n");
    return 1;
  }
  uint16_t patch_enc = rd_be16(patch_ffi + 4);
  uint16_t patch_comp = rd_be16(patch_ffi + 6);
  uint32_t patch_window_size = rd_be32(patch_ffi + 8);
  fprintf(stderr, "patch: enc=%u comp=%u window_size=0x%X\n", patch_enc,
          patch_comp, patch_window_size);
  if (patch_comp != 3) {  // XEX_COMPRESSION_DELTA
    fprintf(stderr, "patch compression type %u not DELTA(3)\n", patch_comp);
    return 1;
  }

  // delta patch descriptor
  uint32_t dd_off = 0;
  const uint8_t* dd = patch.get_opt(0x5FF, &dd_off);
  if (!dd) {
    fprintf(stderr, "patch delta descriptor missing\n");
    return 1;
  }
  uint32_t dd_size = rd_be32(dd + 0x00);              // descriptor size
  uint32_t dd_target_version = rd_be32(dd + 0x04);
  uint32_t dd_source_version = rd_be32(dd + 0x08);
  uint8_t digest_source[0x14];
  memcpy(digest_source, dd + 0x0C, 0x14);
  uint8_t image_key_source[0x10];
  memcpy(image_key_source, dd + 0x20, 0x10);
  uint32_t size_of_target_headers = rd_be32(dd + 0x30);
  uint32_t dh_source_offset = rd_be32(dd + 0x34);
  uint32_t dh_source_size = rd_be32(dd + 0x38);
  uint32_t dh_target_offset = rd_be32(dd + 0x3C);
  uint32_t di_source_offset = rd_be32(dd + 0x40);
  uint32_t di_source_size = rd_be32(dd + 0x44);
  uint32_t di_target_offset = rd_be32(dd + 0x48);
  // info (first delta record) at dd + 0x4C
  const uint8_t* patch_info = dd + 0x4C;
  fprintf(stderr,
          "patch: dd_size=0x%X tgt_ver=0x%X src_ver=0x%X tgt_headers=0x%X\n"
          "  dh src=0x%X+0x%X -> tgt 0x%X | di src=0x%X+0x%X -> tgt 0x%X\n",
          dd_size, dd_target_version, dd_source_version, size_of_target_headers,
          dh_source_offset, dh_source_size, dh_target_offset, di_source_offset,
          di_source_size, di_target_offset);

  const uint8_t* patch_sec = patch.data.data() + patch.security_offset;
  uint8_t patch_aes_key[16];
  memcpy(patch_aes_key, patch_sec + SEC_OFF_AES_KEY, 16);
  fprintf(stderr, "patch: aes=");
  for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", patch_aes_key[i]);
  fprintf(stderr, "\n");

  // ---- ApplyPatch ----
  const uint32_t original_base_address = 0x82000000;  // base load address
  (void)original_base_address;

  // Patch base XEX header (in memory).
  std::vector<uint8_t> base_header(base.data.begin(),
                                   base.data.begin() + base.header_size);

  uint32_t original_image_size = base_image_size;
  uint32_t header_target_size = size_of_target_headers;
  if (!header_target_size) {
    header_target_size = dh_target_offset + dh_source_size;
  }

  if (header_target_size > base_header.size()) {
    base_header.resize(header_target_size, 0);
  }
  uint8_t* header_ptr = base_header.data();

  // If headers_source_offset is set, copy source -> target.
  if (dh_source_offset) {
    memcpy(header_ptr + dh_target_offset, header_ptr + dh_source_offset,
           dh_source_size);
  }
  if (header_target_size < base_header.size()) {
    memset(header_ptr + header_target_size, 0,
           base_header.size() - header_target_size);
  }

  // Apply header delta patch.
  uint32_t headerpatch_size = dd_size;
  int rc = lzxdelta_apply_patch((uint8_t*)patch_info, headerpatch_size,
                                patch_window_size, header_ptr);
  if (rc) {
    fprintf(stderr, "header patch failed rc=%d\n", rc);
    return 1;
  }
  fprintf(stderr, "header delta patch applied OK\n");

  // Re-read security info from patched header.
  const uint8_t* patched_sec = header_ptr + base.security_offset;
  uint32_t new_image_size = rd_be32(patched_sec + SEC_OFF_IMAGE_SIZE);
  uint8_t patched_aes_key[16];
  memcpy(patched_aes_key, patched_sec + SEC_OFF_AES_KEY, 16);
  uint32_t new_base_address = rd_be32(patched_sec + SEC_OFF_LOAD_ADDRESS);
  fprintf(stderr, "patched base: image_size=0x%X base=0x%X aes=", new_image_size,
          new_base_address);
  for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", patched_aes_key[i]);
  fprintf(stderr, "\n");

  // Header patch updated base XEX key; redecrypt to get new base session key.
  uint8_t new_base_session_key[16];
  aes_128_cbc_decrypt(xe_xex2_retail_key, patched_aes_key, 16,
                      new_base_session_key);

  // Decrypt patch XEX key using base key.
  uint8_t patch_session_key[16];
  aes_128_cbc_decrypt(new_base_session_key, patch_aes_key, 16,
                      patch_session_key);

  // Test delta key against decrypted keys.
  uint8_t test_delta_key[16];
  aes_128_cbc_decrypt(new_base_session_key, image_key_source, 16,
                      test_delta_key);
  if (memcmp(test_delta_key, base_session_key, 16) != 0) {
    fprintf(stderr, "WARNING: test delta key mismatch (continued anyway)\n");
  } else {
    fprintf(stderr, "delta key verification PASSED\n");
  }

  // Decrypt patch payload.
  const uint8_t* patch_buffer = patch.data.data() + patch.header_size;
  size_t patch_length = patch.data.size() - patch.header_size;
  std::vector<uint8_t> input_buffer(patch_length, 0);
  if (patch_enc == 1) {
    aes_128_cbc_decrypt(patch_session_key, patch_buffer, patch_length,
                        input_buffer.data());
  } else {
    memcpy(input_buffer.data(), patch_buffer, patch_length);
  }

  // Prepare base_exe image (allocate new_image_size).
  uint64_t final_size = (uint64_t)new_image_size > base_image.size()
                            ? (uint64_t)new_image_size
                            : (uint64_t)base_image.size();
  std::vector<uint8_t> base_exe(final_size, 0);
  memcpy(base_exe.data(), base_image.data(), base_image.size());

  // If image_source_offset set, copy internal.
  if (di_source_offset) {
    memcpy(base_exe.data() + di_target_offset,
           base_exe.data() + di_source_offset, di_source_size);
  }
  uint32_t image_target_size = di_target_offset + di_source_size;
  (void)image_target_size;

  // Loop through each patch block and apply delta patches.
  const uint8_t* cur_block_info = patch_ffi + 0xC;  // normal.first_block
  const uint8_t* pblock = input_buffer.data();
  const uint8_t* input_end = input_buffer.data() + input_buffer.size();

  int block_index = 0;
  while (true) {
    uint32_t block_size = rd_be32(cur_block_info);
    if (!block_size) break;
    const uint8_t* next_block_info = pblock;  // info at current block head describes NEXT block
    if ((size_t)(pblock - input_buffer.data()) + block_size > input_buffer.size()) {
      fprintf(stderr, "block %d out of range\n", block_index);
      return 1;
    }
    // Compare block hash.
    uint8_t digest[0x14];
    sha1_calc(pblock, block_size, digest);
    if (memcmp(digest, cur_block_info + 4, 0x14) != 0) {
      fprintf(stderr, "block %d hash mismatch (bad key?)\n", block_index);
      return 1;
    }
    uint8_t* patch_records = (uint8_t*)pblock + 24;
    uint32_t block_data_size = block_size - 24;
    rc = lzxdelta_apply_patch(patch_records, block_data_size, patch_window_size,
                              base_exe.data());
    if (rc) {
      fprintf(stderr, "image block %d patch failed rc=%d\n", block_index, rc);
      return 1;
    }
    fprintf(stderr, "image block %d applied: size=0x%X\n", block_index,
            block_size);
    pblock += block_size;
    cur_block_info = next_block_info;  // same as xenia: info at CURRENT block head describes NEXT block
    block_index++;
  }

  fprintf(stderr, "TU image synthesized: %u blocks, size=0x%llX\n", block_index,
          (unsigned long long)base_exe.size());

  // Optionally dump the unpatched base flat image for comparison.
  if (dump_base) {
    if (!write_file(base_out, base_image.data(), base_image.size())) {
      fprintf(stderr, "cannot write base dump %s\n", base_out.c_str());
      return 1;
    }
    fprintf(stderr, "written %s (%llu bytes)\n", base_out.c_str(),
            (unsigned long long)base_image.size());
  }

  // Write output flat image.
  if (!write_file(out_path, base_exe.data(), base_exe.size())) {
    fprintf(stderr, "cannot write output %s\n", out_path.c_str());
    return 1;
  }
  fprintf(stderr, "written %s (%llu bytes)\n", out_path.c_str(),
          (unsigned long long)base_exe.size());
  return 0;
}