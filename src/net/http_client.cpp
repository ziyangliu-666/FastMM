#include "fastmm/net/http_client.hpp"

#include <cstring>

namespace fastmm::net::detail {

namespace {

// Parses a chunk-size line "1a3f;ext=1\r\n" starting at `pos`. Returns false while the line
// is incomplete; sets `size` and moves `pos` past the CRLF. Invalid hex -> size = SIZE_MAX.
bool parse_chunk_size(std::string_view in, std::size_t& pos, std::size_t& size) noexcept {
  const std::size_t eol = in.find("\r\n", pos);
  if (eol == std::string_view::npos) return false;
  std::size_t v = 0;
  std::size_t i = pos;
  int digits = 0;
  for (; i < eol; ++i) {
    const char c = in[i];
    int d = -1;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      d = c - 'A' + 10;
    if (d < 0) break;
    if (++digits > 15) {
      size = static_cast<std::size_t>(-1);
      return true;
    }
    v = (v << 4) | static_cast<std::size_t>(d);
  }
  // Anything after the digits must be a chunk extension (";...") or whitespace.
  if (digits == 0 || (i < eol && in[i] != ';' && in[i] != ' ' && in[i] != '\t')) {
    size = static_cast<std::size_t>(-1);
    return true;
  }
  size = v;
  pos = eol + 2;
  return true;
}

}  // namespace

void HttpResponseParser::reset() noexcept {
  stage_ = Stage::Head;
  connection_close_ = false;
  head_len_ = body_len_ = scan_off_ = chunk_remaining_ = expected_len_ = 0;
  error_ = "";
}

HttpResponseParser::Result HttpResponseParser::parse_head(RecvBuffer& rx) noexcept {
  HttpResponseHead head;
  const HttpParseStatus st = parse_response_head(rx.readable_view(), head);
  if (st == HttpParseStatus::Incomplete) return Result::NeedMore;
  if (st == HttpParseStatus::TooManyHeaders) return fail("too many headers");
  if (st == HttpParseStatus::Invalid) return fail("malformed status line or headers");

  if (head.status == 100) {
    // "100 Continue" (or any 1xx) is informational: drop it and parse the real response.
    rx.consume(head.head_len);
    return parse_head(rx);
  }
  head_len_ = head.head_len;
  scan_off_ = head.head_len;
  connection_close_ =
      header_has_token(head.headers.get("Connection"), "close") || head.version == "HTTP/1.0";

  const bool no_body = head_request_ || head.status == 204 || head.status == 304;
  if (no_body) {
    stage_ = Stage::FixedBody;
    expected_len_ = 0;
  } else if (header_has_token(head.headers.get("Transfer-Encoding"), "chunked")) {
    stage_ = Stage::ChunkSize;
  } else if (const std::string_view cl = head.headers.get("Content-Length"); !cl.empty()) {
    if (!parse_content_length(cl, expected_len_)) return fail("bad Content-Length");
    stage_ = Stage::FixedBody;
  } else {
    // RFC 7230 §3.3.3 item 7: body runs until the server closes the connection.
    stage_ = Stage::UntilClose;
    connection_close_ = true;
  }
  return Result::NeedMore;
}

HttpResponseParser::Result HttpResponseParser::parse(RecvBuffer& rx,
                                                     HttpResponse& out,
                                                     std::size_t& consume_len) noexcept {
  if (stage_ == Stage::Head) {
    const Result r = parse_head(rx);
    if (r == Result::Invalid) return r;
    if (stage_ == Stage::Head) return Result::NeedMore;
  }
  const std::string_view in = rx.readable_view();
  std::byte* base = rx.readable().data();

  for (;;) {
    switch (stage_) {
      case Stage::Head:
        return Result::NeedMore;  // unreachable

      case Stage::FixedBody:
        if (in.size() < head_len_ + expected_len_) {
          if (head_len_ + expected_len_ > rx.capacity()) return fail("body exceeds receive buffer");
          return Result::NeedMore;
        }
        body_len_ = expected_len_;
        scan_off_ = head_len_ + expected_len_;
        return finish(rx, out, consume_len);

      case Stage::ChunkSize: {
        std::size_t size = 0;
        std::size_t pos = scan_off_;
        if (!parse_chunk_size(in, pos, size)) return Result::NeedMore;
        if (size == static_cast<std::size_t>(-1)) return fail("bad chunk size");
        scan_off_ = pos;
        if (size == 0) {
          stage_ = Stage::Trailers;
          break;
        }
        chunk_remaining_ = size;
        stage_ = Stage::ChunkData;
        break;
      }

      case Stage::ChunkData: {
        const std::size_t avail = in.size() - scan_off_;
        const std::size_t n = avail < chunk_remaining_ ? avail : chunk_remaining_;
        if (n > 0) {
          // Move chunk bytes left so the decoded body stays contiguous behind the head.
          std::memmove(base + head_len_ + body_len_, base + scan_off_, n);
          body_len_ += n;
          scan_off_ += n;
          chunk_remaining_ -= n;
        }
        if (chunk_remaining_ > 0) {
          if (scan_off_ >= rx.capacity()) return fail("body exceeds receive buffer");
          return Result::NeedMore;
        }
        stage_ = Stage::ChunkEnd;
        break;
      }

      case Stage::ChunkEnd:  // CRLF after the chunk data
        if (in.size() < scan_off_ + 2) return Result::NeedMore;
        if (in[scan_off_] != '\r' || in[scan_off_ + 1] != '\n')
          return fail("missing CRLF after chunk");
        scan_off_ += 2;
        stage_ = Stage::ChunkSize;
        break;

      case Stage::Trailers: {
        // Zero or more "Name: value\r\n" lines, then an empty line. Trailers are ignored.
        for (;;) {
          const std::size_t eol = in.find("\r\n", scan_off_);
          if (eol == std::string_view::npos) return Result::NeedMore;
          const bool blank = eol == scan_off_;
          scan_off_ = eol + 2;
          if (blank) return finish(rx, out, consume_len);
        }
      }

      case Stage::UntilClose:
        body_len_ = in.size() - head_len_;
        scan_off_ = in.size();
        if (in.size() >= rx.capacity()) return fail("body exceeds receive buffer");
        return Result::NeedMore;
    }
  }
}

HttpResponseParser::Result HttpResponseParser::on_eof(RecvBuffer& rx,
                                                      HttpResponse& out,
                                                      std::size_t& consume_len) noexcept {
  if (stage_ != Stage::UntilClose) return fail("connection closed mid-response");
  body_len_ = rx.readable_view().size() - head_len_;
  scan_off_ = rx.readable_view().size();
  return finish(rx, out, consume_len);
}

HttpResponseParser::Result HttpResponseParser::finish(RecvBuffer& rx,
                                                      HttpResponse& out,
                                                      std::size_t& consume_len) noexcept {
  // Re-parse the head so the views point at the buffer's current location (it may have
  // been compacted while the body was arriving).
  HttpResponseHead head;
  if (parse_response_head(rx.readable_view(), head) != HttpParseStatus::Ok)
    return fail("head vanished");
  out.status = head.status;
  out.headers = head.headers;
  out.body = rx.readable_view().substr(head_len_, body_len_);
  out.error = NetError::None;
  consume_len = scan_off_;
  stage_ = Stage::Head;
  return Result::Complete;
}

}  // namespace fastmm::net::detail
