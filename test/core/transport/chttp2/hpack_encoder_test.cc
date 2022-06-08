/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "src/core/ext/transport/chttp2/transport/hpack_encoder.h"

#include <stdio.h>
#include <string.h>

#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/ext/transport/chttp2/transport/hpack_parser.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/resource_quota/resource_quota.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "test/core/util/parse_hexstring.h"
#include "test/core/util/slice_splitter.h"
#include "test/core/util/test_config.h"

#define TEST(x) run_test(x, #x)

static auto* g_memory_allocator = new grpc_core::MemoryAllocator(
    grpc_core::ResourceQuota::Default()->memory_quota()->CreateMemoryAllocator(
        "test"));

grpc_core::HPackCompressor* g_compressor;
int g_failure = 0;

typedef struct {
  bool eof;
  bool use_true_binary_metadata;
} verify_params;

/* verify that the output frames that are generated by encoding the stream
   have sensible type and flags values */
static void verify_frames(grpc_slice_buffer& output, bool header_is_eof) {
  /* per the HTTP/2 spec:
       All frames begin with a fixed 9-octet header followed by a
       variable-length payload.

       +-----------------------------------------------+
       |                 Length (24)                   |
       +---------------+---------------+---------------+
       |   Type (8)    |   Flags (8)   |
       +-+-------------+---------------+-------------------------------+
       |R|                 Stream Identifier (31)                      |
       +=+=============================================================+
       |                   Frame Payload (0...)                      ...
       +---------------------------------------------------------------+
   */
  uint8_t type = 0xff, flags = 0xff;
  size_t i, merged_length, frame_size;
  bool first_frame = false;
  bool in_header = false;
  bool end_header = false;
  bool is_closed = false;
  for (i = 0; i < output.count;) {
    first_frame = i == 0;
    grpc_slice* slice = &output.slices[i++];

    // Read gRPC frame header
    uint8_t* p = GRPC_SLICE_START_PTR(*slice);
    frame_size = 0;
    frame_size |= static_cast<uint32_t>(p[0]) << 16;
    frame_size |= static_cast<uint32_t>(p[1]) << 8;
    frame_size |= static_cast<uint32_t>(p[2]);
    type = p[3];
    flags = p[4];

    // Read remainder of the gRPC frame
    merged_length = GRPC_SLICE_LENGTH(*slice);
    while (merged_length < frame_size + 9) {  // including 9 byte frame header
      grpc_slice* slice = &output.slices[i++];
      merged_length += GRPC_SLICE_LENGTH(*slice);
    }

    // Verifications
    if (first_frame && type != GRPC_CHTTP2_FRAME_HEADER) {
      gpr_log(GPR_ERROR, "expected first frame to be of type header");
      gpr_log(GPR_ERROR, "EXPECT: 0x%x", GRPC_CHTTP2_FRAME_HEADER);
      gpr_log(GPR_ERROR, "GOT:    0x%x", type);
      g_failure = 1;
    } else if (first_frame && header_is_eof &&
               !(flags & GRPC_CHTTP2_DATA_FLAG_END_STREAM)) {
      gpr_log(GPR_ERROR, "missing END_STREAM flag in HEADER frame");
      g_failure = 1;
    }
    if (is_closed &&
        (type == GRPC_CHTTP2_FRAME_DATA || type == GRPC_CHTTP2_FRAME_HEADER)) {
      gpr_log(GPR_ERROR,
              "stream is closed; new frame headers and data are not allowed");
      g_failure = 1;
    }
    if (end_header && (type == GRPC_CHTTP2_FRAME_HEADER ||
                       type == GRPC_CHTTP2_FRAME_CONTINUATION)) {
      gpr_log(GPR_ERROR,
              "frame header is ended; new headers and continuations are not "
              "allowed");
      g_failure = 1;
    }
    if (in_header &&
        (type == GRPC_CHTTP2_FRAME_DATA || type == GRPC_CHTTP2_FRAME_HEADER)) {
      gpr_log(GPR_ERROR,
              "parsing frame header; new headers and data are not allowed");
      g_failure = 1;
    }
    if (flags & ~(GRPC_CHTTP2_DATA_FLAG_END_STREAM |
                  GRPC_CHTTP2_DATA_FLAG_END_HEADERS)) {
      gpr_log(GPR_ERROR, "unexpected frame flags: 0x%x", flags);
      g_failure = 1;
    }

    // Update state
    if (flags & GRPC_CHTTP2_DATA_FLAG_END_HEADERS) {
      in_header = false;
      end_header = true;
    } else if (type == GRPC_CHTTP2_DATA_FLAG_END_HEADERS) {
      in_header = true;
    }
    if (flags & GRPC_CHTTP2_DATA_FLAG_END_STREAM) {
      is_closed = true;
      if (type == GRPC_CHTTP2_FRAME_CONTINUATION) {
        gpr_log(GPR_ERROR, "unexpected END_STREAM flag in CONTINUATION frame");
        g_failure = 1;
      }
    }
  }
}

static void CrashOnAppendError(absl::string_view, const grpc_core::Slice&) {
  abort();
}

/* verify that the output generated by encoding the stream matches the
   hexstring passed in */
static void verify(const verify_params params, const char* expected,
                   size_t nheaders, ...) {
  grpc_slice_buffer output;
  grpc_slice merged;
  grpc_slice expect = parse_hexstring(expected);
  size_t i;
  va_list l;
  auto arena = grpc_core::MakeScopedArena(1024, g_memory_allocator);
  grpc_metadata_batch b(arena.get());

  va_start(l, nheaders);
  for (i = 0; i < nheaders; i++) {
    char* key = va_arg(l, char*);
    char* value = va_arg(l, char*);
    b.Append(key, grpc_core::Slice::FromStaticString(value),
             CrashOnAppendError);
  }
  va_end(l);

  grpc_slice_buffer_init(&output);

  grpc_transport_one_way_stats stats;
  stats = {};
  grpc_core::HPackCompressor::EncodeHeaderOptions hopt{
      0xdeadbeef,                      /* stream_id */
      params.eof,                      /* is_eof */
      params.use_true_binary_metadata, /* use_true_binary_metadata */
      16384,                           /* max_frame_size */
      &stats                           /* stats */
  };
  g_compressor->EncodeHeaders(hopt, b, &output);
  verify_frames(output, params.eof);
  merged = grpc_slice_merge(output.slices, output.count);
  grpc_slice_buffer_destroy_internal(&output);

  if (!grpc_slice_eq(merged, expect)) {
    char* expect_str = grpc_dump_slice(expect, GPR_DUMP_HEX | GPR_DUMP_ASCII);
    char* got_str = grpc_dump_slice(merged, GPR_DUMP_HEX | GPR_DUMP_ASCII);
    gpr_log(GPR_ERROR, "mismatched output for %s", expected);
    gpr_log(GPR_ERROR, "EXPECT: %s", expect_str);
    gpr_log(GPR_ERROR, "GOT:    %s", got_str);
    gpr_free(expect_str);
    gpr_free(got_str);
    g_failure = 1;
  }

  grpc_slice_unref_internal(merged);
  grpc_slice_unref_internal(expect);
}

static void test_basic_headers() {
  verify_params params = {
      false,
      false,
  };
  verify(params, "000005 0104 deadbeef 00 0161 0161", 1, "a", "a");
  verify(params, "00000a 0104 deadbeef 00 0161 0161 00 0162 0163", 2, "a", "a",
         "b", "c");
}

static void verify_continuation_headers(const char* key, const char* value,
                                        bool is_eof) {
  auto arena = grpc_core::MakeScopedArena(1024, g_memory_allocator);
  grpc_slice_buffer output;
  grpc_metadata_batch b(arena.get());
  b.Append(key, grpc_core::Slice::FromStaticString(value), CrashOnAppendError);
  grpc_slice_buffer_init(&output);

  grpc_transport_one_way_stats stats;
  stats = {};
  grpc_core::HPackCompressor::EncodeHeaderOptions hopt = {
      0xdeadbeef, /* stream_id */
      is_eof,     /* is_eof */
      false,      /* use_true_binary_metadata */
      150,        /* max_frame_size */
      &stats /* stats */};
  g_compressor->EncodeHeaders(hopt, b, &output);
  verify_frames(output, is_eof);
  grpc_slice_buffer_destroy_internal(&output);
}

static void test_continuation_headers() {
  char value[200];
  memset(value, 'a', 200);
  value[199] = 0;  // null terminator
  verify_continuation_headers("key", value, true);

  char value2[400];
  memset(value2, 'b', 400);
  value2[399] = 0;  // null terminator
  verify_continuation_headers("key2", value2, true);
}

static void run_test(void (*test)(), const char* name) {
  gpr_log(GPR_INFO, "RUN TEST: %s", name);
  grpc_core::ExecCtx exec_ctx;
  g_compressor = new grpc_core::HPackCompressor();
  test();
  delete g_compressor;
}

int main(int argc, char** argv) {
  grpc_test_only_set_slice_hash_seed(0);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  TEST(test_basic_headers);
  TEST(test_continuation_headers);
  grpc_shutdown();
  return g_failure;
}