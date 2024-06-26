// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "util/uring/uring_pool.h"

#include "base/logging.h"
#include "util/uring/proactor.h"

using namespace std;

namespace util {
namespace uring {

UringPool::~UringPool() {
}

ProactorBase* UringPool::CreateProactor() {
  return new Proactor;
}

void UringPool::InitInThread(unsigned index) {
  Proactor* p = static_cast<Proactor*>(proactor_[index]);
  p->Init(ring_depth_);
}

}  // namespace uring
}  // namespace util
