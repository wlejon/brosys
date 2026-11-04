// Promise-returning natives whose work blocks (disk reads, D-Bus calls that
// may wait on a polkit prompt): the work runs on a thread of its own, and the
// promise settles on the page's thread at the next tickSysAsync.
#pragma once

#include "embed/embed.h"

#include <functional>
#include <string>
#include <vector>

namespace brosys::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

// `work` runs off the page's thread and returns "" or why it failed (the
// promise is rejected with an Error of that message). `result` runs on the
// page's thread after a successful `work` and builds the resolution value.
// Both may share state through captured shared_ptrs.
Value runAsync(std::function<std::string()> work, std::function<Value()> result);

// Settles the finished jobs' promises (page thread).
void drainAsyncJobs();
// Waits for every job still running and drops it unsettled (shutdown).
void joinAsyncJobs();

// An Error object with `message`.
Value makeError(const std::string& message);

// Object.keys(obj) as strings (empty for a non-object).
std::vector<std::string> objectKeys(Value obj);
bool isArray(Value v);

}  // namespace brosys::api
