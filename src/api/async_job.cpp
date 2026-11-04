#include "async_job.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace brosys::api {

namespace {

struct AsyncJob {
    std::shared_ptr<ev::Persistent> promise;
    std::function<std::string()> work;
    std::function<Value()> result;
    std::string error;
    std::atomic<bool> done{false};
    std::thread worker;
    // The thread whose realm made the promise: only its tick settles it.
    std::thread::id owner;
};

std::mutex g_jobs_mu;
std::vector<std::shared_ptr<AsyncJob>> g_jobs;

std::vector<std::shared_ptr<AsyncJob>> takeJobs(bool finishedOnly) {
    std::lock_guard<std::mutex> lock(g_jobs_mu);
    std::vector<std::shared_ptr<AsyncJob>> taken, kept;
    const std::thread::id self = std::this_thread::get_id();
    for (auto& j : g_jobs) {
        if (j->owner == self && (!finishedOnly || j->done.load(std::memory_order_acquire)))
            taken.push_back(std::move(j));
        else kept.push_back(std::move(j));
    }
    g_jobs = std::move(kept);
    return taken;
}

}  // namespace

Value makeError(const std::string& message) {
    ev::Persistent text(ev::fromUtf8(message));
    auto ctor = ev::globalValue("Error");
    if (ctor.found && ev::isFunction(ctor.value)) {
        ev::Persistent c(ctor.value);
        const Value arg = text.get();
        auto r = ev::construct(c.get(), std::span<const Value>(&arg, 1));
        if (!r.thrown) return r.value;
    }
    return text.get();
}

Value runAsync(std::function<std::string()> work, std::function<Value()> result) {
    auto job = std::make_shared<AsyncJob>();
    job->promise = std::make_shared<ev::Persistent>(ev::createPromise());
    job->work = std::move(work);
    job->result = std::move(result);
    job->owner = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lock(g_jobs_mu);
        g_jobs.push_back(job);
    }
    AsyncJob* raw = job.get();
    job->worker = std::thread([raw] {
        try {
            raw->error = raw->work();
        } catch (const std::exception& e) {
            raw->error = e.what();
        } catch (...) {
            raw->error = "unknown error";
        }
        raw->done.store(true, std::memory_order_release);
    });
    return job->promise->get();
}

void drainAsyncJobs() {
    for (auto& job : takeJobs(true)) {
        if (job->worker.joinable()) job->worker.join();
        if (!job->error.empty()) {
            ev::Persistent err(makeError(job->error));
            ev::rejectPromise(job->promise->get(), err.get());
        } else {
            ev::Persistent v(job->result ? job->result() : ev::undefined());
            ev::resolvePromise(job->promise->get(), v.get());
        }
    }
}

void joinAsyncJobs() {
    for (auto& job : takeJobs(false))
        if (job->worker.joinable()) job->worker.join();
}

std::vector<std::string> objectKeys(Value obj) {
    std::vector<std::string> out;
    if (!ev::isObject(obj)) return out;
    ev::Persistent target(obj);
    ev::GlobalValue objectCtor = ev::globalValue("Object");
    if (!objectCtor.found) return out;
    ev::Persistent ctor(objectCtor.value);
    ev::Persistent keysFn(ev::getProperty(ctor.get(), "keys"));
    const Value args[1] = {target.get()};
    ev::CallResult r = ev::call(keysFn.get(), ctor.get(), std::span<const Value>(args, 1));
    if (r.thrown) return out;
    ev::Persistent arr(r.value);
    const Value lenV = ev::getProperty(arr.get(), "length");
    const uint32_t n = ev::isNumber(lenV) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        Value k = ev::getElement(arr.get(), i);
        if (ev::isString(k)) out.push_back(ev::toUtf8(k));
    }
    return out;
}

bool isArray(Value v) {
    if (!ev::isObject(v)) return false;
    ev::Persistent target(v);
    ev::GlobalValue arrayCtor = ev::globalValue("Array");
    if (!arrayCtor.found) return false;
    ev::Persistent ctor(arrayCtor.value);
    ev::Persistent fn(ev::getProperty(ctor.get(), "isArray"));
    const Value args[1] = {target.get()};
    ev::CallResult r = ev::call(fn.get(), ctor.get(), std::span<const Value>(args, 1));
    return !r.thrown && ev::isBool(r.value) && ev::toBool(r.value);
}

}  // namespace brosys::api
