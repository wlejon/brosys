#include "api.h"
#include "arg_reader.h"
#include "object_builder.h"

#include <string>
#include <vector>

namespace brosys::api {

namespace {

Value buildProcessInfo(const ProcessInfo& p) {
    ObjectBuilder b;
    b.set("pid", p.pid);
    b.set("ppid", p.ppid);
    b.set("name", p.name);
    b.set("cmdline", p.cmdline);
    b.set("state", p.state);
    b.set("user", p.user);
    b.set("uid", p.uid);
    b.set("rssBytes", static_cast<double>(p.rss_bytes));
    b.set("rss_bytes", static_cast<double>(p.rss_bytes));
    b.set("vszBytes", static_cast<double>(p.vsz_bytes));
    b.set("vsz_bytes", static_cast<double>(p.vsz_bytes));
    b.set("cpuTimeMs", static_cast<double>(p.cpu_time_ms));
    b.set("cpu_time_ms", static_cast<double>(p.cpu_time_ms));
    b.set("threads", p.threads);
    return b.build();
}

Value doListProcesses(Value, std::span<const Value>) {
    ProcessService* svc = getProcessService();
    std::vector<ProcessInfo> procs = svc ? svc->list() : list_processes();
    ArrayBuilder arr(procs.size());
    for (size_t i = 0; i < procs.size(); ++i) {
        ev::Persistent item(buildProcessInfo(procs[i]));
        arr.set(static_cast<uint32_t>(i), item.get());
    }
    return arr.build();
}

Value doKillProcess(Value, std::span<const Value> args) {
    if (args.empty() || !ev::isNumber(args[0])) {
        return ev::throwTypeError("killProcess requires a numeric pid");
    }

    ev::Persistent arg0(args[0]);
    ev::Persistent arg1(args.size() > 1 ? args[1] : ev::undefined());

    const int32_t pid = i32Val(arg0.get());
    int32_t sig = signal_num::SIGTERM_NUM;

    const Value v1 = arg1.get();
    if (!ev::isUndefined(v1) && !ev::isNull(v1)) {
        if (ev::isBool(v1)) {
            sig = ev::toBool(v1) ? signal_num::SIGKILL_NUM : signal_num::SIGTERM_NUM;
        } else if (ev::isNumber(v1)) {
            sig = i32Val(v1);
        } else if (ev::isString(v1)) {
            const std::string s = strVal(v1);
            sig = parse_signal(s);
            if (sig == 0) {
                return ev::throwTypeError("Unknown signal: " + s);
            }
        } else {
            return ev::throwTypeError("Invalid signal argument");
        }
    }

    ProcessService* svc = getProcessService();
    Result r = svc ? svc->kill(pid, sig) : kill_process(pid, sig);
    if (!r.ok) {
        return ev::throwError("killProcess failed: " + r.error);
    }
    return ev::fromBool(true);
}

}  // namespace

void installProcesses(ObjectBuilder& sys) {
    sys.def("listProcesses", 0, doListProcesses);
    sys.def("killProcess", 2, doKillProcess);

    ObjectBuilder procs;
    procs.def("list", 0, doListProcesses);
    procs.def("kill", 2, doKillProcess);
    sys.set("processes", procs.build());
}

void shutdownProcesses() {
}

}  // namespace brosys::api
