#include "westmere/begin_implementation.h"
#include "westmere/dom_parser_implementation.h"
#include "generic/stage2/jsoncharutils.h"

//
// Stage 1
//

namespace {
namespace SIMDJSON_IMPLEMENTATION {

using namespace simd;

struct json_character_block {
  static really_inline json_character_block classify(const simd::simd8x64<uint8_t>& in);

  really_inline uint64_t whitespace() const { return _whitespace; }
  really_inline uint64_t op() const { return _op; }
  really_inline uint64_t scalar() { return ~(op() | whitespace()); }

  uint64_t _whitespace;
  uint64_t _op;
};

really_inline json_character_block json_character_block::classify(const simd::simd8x64<uint8_t>& in) {
  // These lookups rely on the fact that anything < 127 will match the lower 4 bits, which is why
  // we can't use the generic lookup_16.
  auto whitespace_table = simd8<uint8_t>::repeat_16(' ', 100, 100, 100, 17, 100, 113, 2, 100, '\t', '\n', 112, 100, '\r', 100, 100);
  auto op_table = simd8<uint8_t>::repeat_16(',', '}', 0, 0, 0xc0u, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{');

  // We compute whitespace and op separately. If the code later only use one or the
  // other, given the fact that all functions are aggressively inlined, we can
  // hope that useless computations will be omitted. This is namely case when
  // minifying (we only need whitespace).

  uint64_t whitespace = simd8x64<bool>(
        in.chunks[0] == simd8<uint8_t>(_mm_shuffle_epi8(whitespace_table, in.chunks[0])),
        in.chunks[1] == simd8<uint8_t>(_mm_shuffle_epi8(whitespace_table, in.chunks[1])),
        in.chunks[2] == simd8<uint8_t>(_mm_shuffle_epi8(whitespace_table, in.chunks[2])),
        in.chunks[3] == simd8<uint8_t>(_mm_shuffle_epi8(whitespace_table, in.chunks[3]))
  ).to_bitmask();

  // | 32 handles the fact that { } and [ ] are exactly 32 bytes apart
  uint64_t op = simd8x64<bool>(
        (in.chunks[0] | 32) == simd8<uint8_t>(_mm_shuffle_epi8(op_table, in.chunks[0]-',')),
        (in.chunks[1] | 32) == simd8<uint8_t>(_mm_shuffle_epi8(op_table, in.chunks[1]-',')),
        (in.chunks[2] | 32) == simd8<uint8_t>(_mm_shuffle_epi8(op_table, in.chunks[2]-',')),
        (in.chunks[3] | 32) == simd8<uint8_t>(_mm_shuffle_epi8(op_table, in.chunks[3]-','))
  ).to_bitmask();
  return { whitespace, op };
}

really_inline bool is_ascii(const simd8x64<uint8_t>& input) {
  return input.reduce_or().is_ascii();
}

UNUSED really_inline simd8<bool> must_be_continuation(const simd8<uint8_t> prev1, const simd8<uint8_t> prev2, const simd8<uint8_t> prev3) {
  simd8<uint8_t> is_second_byte = prev1.saturating_sub(0b11000000u-1); // Only 11______ will be > 0
  simd8<uint8_t> is_third_byte  = prev2.saturating_sub(0b11100000u-1); // Only 111_____ will be > 0
  simd8<uint8_t> is_fourth_byte = prev3.saturating_sub(0b11110000u-1); // Only 1111____ will be > 0
  // Caller requires a bool (all 1's). All values resulting from the subtraction will be <= 64, so signed comparison is fine.
  return simd8<int8_t>(is_second_byte | is_third_byte | is_fourth_byte) > int8_t(0);
}

really_inline simd8<bool> must_be_2_3_continuation(const simd8<uint8_t> prev2, const simd8<uint8_t> prev3) {
  simd8<uint8_t> is_third_byte  = prev2.saturating_sub(0b11100000u-1); // Only 111_____ will be > 0
  simd8<uint8_t> is_fourth_byte = prev3.saturating_sub(0b11110000u-1); // Only 1111____ will be > 0
  // Caller requires a bool (all 1's). All values resulting from the subtraction will be <= 64, so signed comparison is fine.
  return simd8<int8_t>(is_third_byte | is_fourth_byte) > int8_t(0);
}

} // namespace SIMDJSON_IMPLEMENTATION
} // unnamed namespace

#include "generic/stage1/utf8_lookup4_algorithm.h"
#include "generic/stage1/json_structural_indexer.h"
#include "generic/stage1/utf8_validator.h"

//
// Stage 2
//
#include "westmere/stringparsing.h"
#include "westmere/numberparsing.h"
#include "generic/stage2/structural_parser.h"

//
// Implementation-specific overrides
//

namespace {
namespace SIMDJSON_IMPLEMENTATION {
namespace stage1 {

really_inline uint64_t json_string_scanner::find_escaped(uint64_t backslash) {
  if (!backslash) { uint64_t escaped = prev_escaped; prev_escaped = 0; return escaped; }
  return find_escaped_branchless(backslash);
}

} // namespace stage1

WARN_UNUSED error_code implementation::minify(const uint8_t *buf, size_t len, uint8_t *dst, size_t &dst_len) const noexcept {
  return westmere::stage1::json_minifier::minify<64>(buf, len, dst, dst_len);
}

WARN_UNUSED error_code dom_parser_implementation::stage1(const uint8_t *_buf, size_t _len, bool streaming) noexcept {
  this->buf = _buf;
  this->len = _len;
  return westmere::stage1::json_structural_indexer::index<64>(_buf, _len, *this, streaming);
}

WARN_UNUSED bool implementation::validate_utf8(const char *buf, size_t len) const noexcept {
  return westmere::stage1::generic_validate_utf8(buf,len);
}

WARN_UNUSED error_code dom_parser_implementation::stage2(dom::document &_doc) noexcept {
  if (auto error = stage2::parse_structurals<false>(*this, _doc)) { return error; }

  // If we didn't make it to the end, it's an error
  if ( next_structural_index != n_structural_indexes ) {
    logger::log_string("More than one JSON value at the root of the document, or extra characters at the end of the JSON!");
    return TAPE_ERROR;
  }

  return SUCCESS;
}

WARN_UNUSED error_code dom_parser_implementation::stage2_next(dom::document &_doc) noexcept {
  return stage2::parse_structurals<true>(*this, _doc);
}

WARN_UNUSED error_code dom_parser_implementation::parse(const uint8_t *_buf, size_t _len, dom::document &_doc) noexcept {
  auto error = stage1(_buf, _len, false);
  if (error) { return error; }
  return stage2(_doc);
}

} // namespace SIMDJSON_IMPLEMENTATION
} // unnamed namespace

#include "westmere/end_implementation.h"
