// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//
#include "util/cloud/s3_file.h"

#include <absl/strings/str_cat.h>

#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/dynamic_body.hpp>

#include "base/logging.h"
#include "util/cloud/cloud_utils.h"

using namespace std;

namespace h2 = boost::beast::http;

namespace util {
namespace cloud {

namespace {

inline void SetRange(size_t from, size_t to, h2::fields* flds) {
  string tmp = absl::StrCat("bytes=", from, "-");
  if (to < kuint64max) {
    absl::StrAppend(&tmp, to - 1);
  }
  flds->set(h2::field::range, std::move(tmp));
}

inline string_view ToSv(const boost::string_view s) {
  return string_view{s.data(), s.size()};
}

std::ostream& operator<<(std::ostream& os, const h2::response<h2::buffer_body>& msg) {
  os << msg.reason() << std::endl;
  for (const auto& f : msg) {
    os << f.name_string() << " : " << f.value() << std::endl;
  }
  os << "-------------------------";

  return os;
}

class S3ReadFile : public io::ReadonlyFile {
 public:
  // does not own pool object, only wraps it with ReadonlyFile interface.
  S3ReadFile(AWS* aws, http::Client* client, string read_obj_url)
      : aws_(*aws), client_(client), read_obj_url_(std::move(read_obj_url)) {
  }

  virtual ~S3ReadFile() final;

  // Reads upto length bytes and updates the result to point to the data.
  // May use buffer for storing data. In case, EOF reached sets result.size() < length but still
  // returns Status::OK.
  io::Result<size_t> Read(size_t offset, const iovec* v, uint32_t len) final;

  std::error_code Open();

  // releases the system handle for this file.
  std::error_code Close() final;

  size_t Size() const final {
    return size_;
  }

  int Handle() const final {
    return -1;
  }

 private:
  HttpParser* parser() {
    return &parser_;
  }

  AWS& aws_;
  http::Client* client_;

  const string read_obj_url_;

  HttpParser parser_;
  size_t size_ = 0, offs_ = 0;
};

S3ReadFile::~S3ReadFile() {
  Close();
}

std::error_code S3ReadFile::Open() {
  string url = absl::StrCat("/", read_obj_url_);
  h2::request<h2::empty_body> req{h2::verb::get, url, 11};

  if (offs_)
    SetRange(offs_, kuint64max, &req);

  VLOG(1) << "Unsigned request: " << req;

  error_code ec = SendRequest(&aws_, client_, &req, &parser_);

  if (ec) {
    return ec;
  }

  CHECK(parser_.keep_alive()) << "TBD";
  const auto& msg = parser_.get();

  VLOG(1) << "HeaderResp("
          << "): " << msg;

  auto content_len_it = msg.find(h2::field::content_length);
  if (content_len_it != msg.end()) {
    size_t content_sz = 0;
    CHECK(absl::SimpleAtoi(ToSv(content_len_it->value()), &content_sz));

    if (size_) {
      CHECK_EQ(size_, content_sz + offs_) << "File size has changed underneath during reopen";
    } else {
      size_ = content_sz;
    }
  }

  return ec;
}

error_code S3ReadFile::Close() {
  return error_code{};
}

io::Result<size_t> S3ReadFile::Read(size_t offset, const iovec* v, uint32_t len) {
  if (offset != offs_) {
    return nonstd::make_unexpected(make_error_code(errc::invalid_argument));
  }

  // We can not cache parser() into local var because Open() below recreates the parser instance.
  if (parser_.is_done()) {
    return 0;
  }

  size_t index = 0;
  size_t read_sofar = 0;

  while (index < len) {
    // We keep body references inside the loop because Open() that might be called here,
    // will recreate the parser from the point the connections disconnected.
    auto& body = parser()->get().body();
    auto& left_available = body.size;
    body.data = v[index].iov_base;
    left_available = v[index].iov_len;

    boost::system::error_code ec = client_->Recv(parser());  // decreases left_available.
    size_t http_read = v[index].iov_len - left_available;

    if (!ec || ec == h2::error::need_buffer) {  // Success
      DVLOG(2) << "Read " << http_read << " bytes from " << offset << " with capacity "
               << v[index].iov_len << "ec: " << ec;

      CHECK(left_available == 0 || !ec);

      // This check does not happen. See here why: https://github.com/boostorg/beast/issues/1662
      // DCHECK_EQ(sz_read, http_read) << " " << range.size() << "/" << left_available;
      offs_ += http_read;
      read_sofar += http_read;
      ++index;

      continue;
    }

    if (ec == h2::error::partial_message) {
      offs_ += http_read;
      VLOG(1) << "Got partial_message";

      // advance the destination buffer as well.
      read_sofar += http_read;
      break;
    }


    LOG(ERROR) << "ec: " << ec << "/" << ec.message() << " at " << offset << "/" << size_;
    return nonstd::make_unexpected(ec);
  }

  return read_sofar;
}

}  // namespace

io::Result<io::ReadonlyFile*> OpenS3ReadFile(string_view path, AWS* aws, http::Client* client,
                                             const io::ReadonlyFile::Options& opts) {
  CHECK(opts.sequential && client);

  string_view bucket, obj_path;

  string read_obj_url{path};
  unique_ptr<S3ReadFile> fl(new S3ReadFile(aws, client, std::move(read_obj_url)));

  auto ec = fl->Open();
  if (ec)
    return nonstd::make_unexpected(ec);

  return fl.release();
}

}  // namespace cloud
}  // namespace util