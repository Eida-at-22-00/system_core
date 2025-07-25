/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "init.h"

#include <dirent.h>
#include <fcntl.h>
#include <paths.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/thread_annotations.h>
#include <fs_avb/fs_avb.h>
#include <fs_mgr_vendor_overlay.h>
#include <libavb/libavb.h>
#include <libgsi/libgsi.h>
#include <libsnapshot/snapshot.h>
#include <logwrap/logwrap.h>
#include <processgroup/processgroup.h>
#include <processgroup/setup.h>
#include <selinux/android.h>
#include <unwindstack/AndroidUnwinder.h>

#include "action.h"
#include "action_manager.h"
#include "action_parser.h"
#include "apex_init_util.h"
#include "epoll.h"
#include "first_stage_init.h"
#include "first_stage_mount.h"
#include "import_parser.h"
#include "keychords.h"
#include "lmkd_service.h"
#include "mount_handler.h"
#include "mount_namespace.h"
#include "property_service.h"
#include "proto_utils.h"
#include "reboot.h"
#include "reboot_utils.h"
#include "second_stage_resources.h"
#include "security.h"
#include "selabel.h"
#include "selinux.h"
#include "service.h"
#include "service_list.h"
#include "service_parser.h"
#include "sigchld_handler.h"
#include "snapuserd_transition.h"
#include "subcontext.h"
#include "system/core/init/property_service.pb.h"
#include "tradeinmode.h"
#include "util.h"

#ifndef RECOVERY
#include "com_android_apex.h"
#endif  // RECOVERY

using namespace std::chrono_literals;
using namespace std::string_literals;

using android::base::boot_clock;
using android::base::ConsumePrefix;
using android::base::GetProperty;
using android::base::GetIntProperty;
using android::base::GetBoolProperty;
using android::base::ReadFileToString;
using android::base::SetProperty;
using android::base::StringPrintf;
using android::base::Timer;
using android::base::Trim;
using android::base::unique_fd;
using android::fs_mgr::AvbHandle;
using android::snapshot::SnapshotManager;
using android::base::WaitForProperty;
using android::base::WriteStringToFile;

namespace android {
namespace init {

static int property_triggers_enabled = 0;

static int sigterm_fd = -1;
static int property_fd = -1;

struct PendingControlMessage {
    std::string message;
    std::string name;
    pid_t pid;
    int fd;
};
static std::mutex pending_control_messages_lock;
static std::queue<PendingControlMessage> pending_control_messages;

// Init epolls various FDs to wait for various inputs.  It previously waited on property changes
// with a blocking socket that contained the information related to the change, however, it was easy
// to fill that socket and deadlock the system.  Now we use locks to handle the property changes
// directly in the property thread, however we still must wake the epoll to inform init that there
// is a change to process, so we use this FD.  It is non-blocking, since we do not care how many
// times WakeMainInitThread() is called, only that the epoll will wake.
static int wake_main_thread_fd = -1;
static void InstallInitNotifier(Epoll* epoll) {
    wake_main_thread_fd = eventfd(0, EFD_CLOEXEC);
    if (wake_main_thread_fd == -1) {
        PLOG(FATAL) << "Failed to create eventfd for waking init";
    }
    auto clear_eventfd = [] {
        uint64_t counter;
        TEMP_FAILURE_RETRY(read(wake_main_thread_fd, &counter, sizeof(counter)));
    };

    if (auto result = epoll->RegisterHandler(wake_main_thread_fd, clear_eventfd); !result.ok()) {
        LOG(FATAL) << result.error();
    }
}

static void WakeMainInitThread() {
    uint64_t counter = 1;
    TEMP_FAILURE_RETRY(write(wake_main_thread_fd, &counter, sizeof(counter)));
}

static class PropWaiterState {
  public:
    bool StartWaiting(const char* name, const char* value) {
        auto lock = std::lock_guard{lock_};
        if (waiting_for_prop_) {
            return false;
        }
        if (GetProperty(name, "") != value) {
            // Current property value is not equal to expected value
            wait_prop_name_ = name;
            wait_prop_value_ = value;
            waiting_for_prop_.reset(new Timer());
        } else {
            LOG(INFO) << "start_waiting_for_property(\"" << name << "\", \"" << value
                      << "\"): already set";
        }
        return true;
    }

    void ResetWaitForProp() {
        auto lock = std::lock_guard{lock_};
        ResetWaitForPropLocked();
    }

    void CheckAndResetWait(const std::string& name, const std::string& value) {
        auto lock = std::lock_guard{lock_};
        // We always record how long init waited for ueventd to tell us cold boot finished.
        // If we aren't waiting on this property, it means that ueventd finished before we even
        // started to wait.
        if (name == kColdBootDoneProp) {
            auto time_waited = waiting_for_prop_ ? waiting_for_prop_->duration().count() : 0;
            std::thread([time_waited] {
                SetProperty("ro.boottime.init.cold_boot_wait", std::to_string(time_waited));
            }).detach();
        }

        if (waiting_for_prop_) {
            if (wait_prop_name_ == name && wait_prop_value_ == value) {
                LOG(INFO) << "Wait for property '" << wait_prop_name_ << "=" << wait_prop_value_
                          << "' took " << *waiting_for_prop_;
                ResetWaitForPropLocked();
                WakeMainInitThread();
            }
        }
    }

    // This is not thread safe because it releases the lock when it returns, so the waiting state
    // may change.  However, we only use this function to prevent running commands in the main
    // thread loop when we are waiting, so we do not care about false positives; only false
    // negatives.  StartWaiting() and this function are always called from the same thread, so false
    // negatives are not possible and therefore we're okay.
    bool MightBeWaiting() {
        auto lock = std::lock_guard{lock_};
        return static_cast<bool>(waiting_for_prop_);
    }

  private:
    void ResetWaitForPropLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_) {
        wait_prop_name_.clear();
        wait_prop_value_.clear();
        waiting_for_prop_.reset();
    }

    std::mutex lock_;
    GUARDED_BY(lock_) std::unique_ptr<Timer> waiting_for_prop_{nullptr};
    GUARDED_BY(lock_) std::string wait_prop_name_;
    GUARDED_BY(lock_) std::string wait_prop_value_;

} prop_waiter_state;

bool start_waiting_for_property(const char* name, const char* value) {
    return prop_waiter_state.StartWaiting(name, value);
}

void ResetWaitForProp() {
    prop_waiter_state.ResetWaitForProp();
}

static class ShutdownState {
  public:
    void TriggerShutdown(const std::string& command) {
        // We can't call HandlePowerctlMessage() directly in this function,
        // because it modifies the contents of the action queue, which can cause the action queue
        // to get into a bad state if this function is called from a command being executed by the
        // action queue.  Instead we set this flag and ensure that shutdown happens before the next
        // command is run in the main init loop.
        auto lock = std::lock_guard{shutdown_command_lock_};
        shutdown_command_ = command;
        do_shutdown_ = true;
        WakeMainInitThread();
    }

    std::optional<std::string> CheckShutdown() __attribute__((warn_unused_result)) {
        auto lock = std::lock_guard{shutdown_command_lock_};
        if (do_shutdown_ && !IsShuttingDown()) {
            do_shutdown_ = false;
            return shutdown_command_;
        }
        return {};
    }

  private:
    std::mutex shutdown_command_lock_;
    std::string shutdown_command_ GUARDED_BY(shutdown_command_lock_);
    bool do_shutdown_ = false;
} shutdown_state;

void DumpState() {
    ServiceList::GetInstance().DumpState();
    ActionManager::GetInstance().DumpState();
}

Parser CreateParser(ActionManager& action_manager, ServiceList& service_list) {
    Parser parser;

    parser.AddSectionParser("service",
                            std::make_unique<ServiceParser>(&service_list, GetSubcontext()));
    parser.AddSectionParser("on", std::make_unique<ActionParser>(&action_manager, GetSubcontext()));
    parser.AddSectionParser("import", std::make_unique<ImportParser>(&parser));

    return parser;
}

#ifndef RECOVERY
template <typename T>
struct LibXmlErrorHandler {
    T handler_;
    template <typename Handler>
    LibXmlErrorHandler(Handler&& handler) : handler_(std::move(handler)) {
        xmlSetGenericErrorFunc(nullptr, &ErrorHandler);
    }
    ~LibXmlErrorHandler() { xmlSetGenericErrorFunc(nullptr, nullptr); }
    static void ErrorHandler(void*, const char* msg, ...) {
        va_list args;
        va_start(args, msg);
        char* formatted;
        if (vasprintf(&formatted, msg, args) >= 0) {
            LOG(ERROR) << formatted;
        }
        free(formatted);
        va_end(args);
    }
};

template <typename Handler>
LibXmlErrorHandler(Handler&&) -> LibXmlErrorHandler<Handler>;
#endif  // RECOVERY

// Returns a Parser that accepts scripts from APEX modules. It supports `service` and `on`.
Parser CreateApexConfigParser(ActionManager& action_manager, ServiceList& service_list) {
    Parser parser;
    auto subcontext = GetSubcontext();
#ifndef RECOVERY
    if (subcontext) {
        const auto apex_info_list_file = "/apex/apex-info-list.xml";
        auto error_handler = LibXmlErrorHandler([&](const auto& error_message) {
            LOG(ERROR) << "Failed to read " << apex_info_list_file << ":" << error_message;
        });
        const auto apex_info_list = com::android::apex::readApexInfoList(apex_info_list_file);
        if (apex_info_list.has_value()) {
            std::vector<std::string> subcontext_apexes;
            for (const auto& info : apex_info_list->getApexInfo()) {
                if (subcontext->PartitionMatchesSubcontext(info.getPartition())) {
                    subcontext_apexes.push_back(info.getModuleName());
                }
            }
            subcontext->SetApexList(std::move(subcontext_apexes));
        }
    }
#endif  // RECOVERY
    parser.AddSectionParser("service", std::make_unique<ServiceParser>(&service_list, subcontext));
    parser.AddSectionParser("on", std::make_unique<ActionParser>(&action_manager, subcontext));

    return parser;
}

static void LoadBootScripts(ActionManager& action_manager, ServiceList& service_list) {
    Parser parser = CreateParser(action_manager, service_list);

    std::string bootscript = GetProperty("ro.boot.init_rc", "");
    if (bootscript.empty()) {
        parser.ParseConfig("/system/etc/init/hw/init.rc");
        if (!parser.ParseConfig("/system/etc/init")) {
            late_import_paths.emplace_back("/system/etc/init");
        }
        // late_import is available only in Q and earlier release. As we don't
        // have system_ext in those versions, skip late_import for system_ext.
        parser.ParseConfig("/system_ext/etc/init");
        if (!parser.ParseConfig("/vendor/etc/init")) {
            late_import_paths.emplace_back("/vendor/etc/init");
        }
        if (!parser.ParseConfig("/odm/etc/init")) {
            late_import_paths.emplace_back("/odm/etc/init");
        }
        if (!parser.ParseConfig("/product/etc/init")) {
            late_import_paths.emplace_back("/product/etc/init");
        }
    } else {
        parser.ParseConfig(bootscript);
    }
}

void PropertyChanged(const std::string& name, const std::string& value) {
    // If the property is sys.powerctl, we bypass the event queue and immediately handle it.
    // This is to ensure that init will always and immediately shutdown/reboot, regardless of
    // if there are other pending events to process or if init is waiting on an exec service or
    // waiting on a property.
    // In non-thermal-shutdown case, 'shutdown' trigger will be fired to let device specific
    // commands to be executed.
    if (name == "sys.powerctl") {
        trigger_shutdown(value);
    }

    if (property_triggers_enabled) {
        ActionManager::GetInstance().QueuePropertyChange(name, value);
        WakeMainInitThread();
    }

    prop_waiter_state.CheckAndResetWait(name, value);
}

static std::optional<boot_clock::time_point> HandleProcessActions() {
    std::optional<boot_clock::time_point> next_process_action_time;
    for (const auto& s : ServiceList::GetInstance()) {
        if ((s->flags() & SVC_RUNNING) && s->timeout_period()) {
            auto timeout_time = s->time_started() + *s->timeout_period();
            if (boot_clock::now() > timeout_time) {
                s->Timeout();
            } else {
                if (!next_process_action_time || timeout_time < *next_process_action_time) {
                    next_process_action_time = timeout_time;
                }
            }
        }

        if (!(s->flags() & SVC_RESTARTING)) continue;

        auto restart_time = s->time_started() + s->restart_period();
        if (boot_clock::now() > restart_time) {
            if (auto result = s->Start(); !result.ok()) {
                LOG(ERROR) << "Could not restart process '" << s->name() << "': " << result.error();
            }
        } else {
            if (!next_process_action_time || restart_time < *next_process_action_time) {
                next_process_action_time = restart_time;
            }
        }
    }
    return next_process_action_time;
}

static Result<void> DoControlStart(Service* service) {
    return service->Start();
}

static Result<void> DoControlStop(Service* service) {
    service->Stop();
    return {};
}

static Result<void> DoControlRestart(Service* service) {
    service->Restart();
    return {};
}

int StopServicesFromApex(const std::string& apex_name) {
    auto services = ServiceList::GetInstance().FindServicesByApexName(apex_name);
    if (services.empty()) {
        LOG(INFO) << "No service found for APEX: " << apex_name;
        return 0;
    }
    std::set<std::string> service_names;
    for (const auto& service : services) {
        service_names.emplace(service->name());
    }
    constexpr std::chrono::milliseconds kServiceStopTimeout = 10s;
    int still_running = StopServicesAndLogViolations(service_names, kServiceStopTimeout,
                        true /*SIGTERM*/);
    // Send SIGKILL to ones that didn't terminate cleanly.
    if (still_running > 0) {
        still_running = StopServicesAndLogViolations(service_names, 0ms, false /*SIGKILL*/);
    }
    return still_running;
}

void RemoveServiceAndActionFromApex(const std::string& apex_name) {
    // Remove services and actions that match apex name
    ActionManager::GetInstance().RemoveActionIf([&](const std::unique_ptr<Action>& action) -> bool {
        if (GetApexNameFromFileName(action->filename()) == apex_name) {
            return true;
        }
        return false;
    });
    ServiceList::GetInstance().RemoveServiceIf([&](const std::unique_ptr<Service>& s) -> bool {
        if (GetApexNameFromFileName(s->filename()) == apex_name) {
            return true;
        }
        return false;
    });
}

static Result<void> DoUnloadApex(const std::string& apex_name) {
    if (StopServicesFromApex(apex_name) > 0) {
        return Error() << "Unable to stop all service from " << apex_name;
    }
    RemoveServiceAndActionFromApex(apex_name);
    return {};
}

static Result<void> UpdateApexLinkerConfig(const std::string& apex_name) {
    // Do not invoke linkerconfig when there's no bin/ in the apex.
    const std::string bin_path = "/apex/" + apex_name + "/bin";
    if (access(bin_path.c_str(), R_OK) != 0) {
        return {};
    }
    const char* linkerconfig_binary = "/apex/com.android.runtime/bin/linkerconfig";
    const char* linkerconfig_target = "/linkerconfig";
    const char* arguments[] = {linkerconfig_binary, "--target", linkerconfig_target, "--apex",
                               apex_name.c_str(),   "--strict"};

    if (logwrap_fork_execvp(arraysize(arguments), arguments, nullptr, false, LOG_KLOG, false,
                            nullptr) != 0) {
        return ErrnoError() << "failed to execute linkerconfig";
    }
    LOG(INFO) << "Generated linker configuration for " << apex_name;
    return {};
}

static Result<void> DoLoadApex(const std::string& apex_name) {
    if (auto result = ParseRcScriptsFromApex(apex_name); !result.ok()) {
        return result.error();
    }

    if (auto result = UpdateApexLinkerConfig(apex_name); !result.ok()) {
        return result.error();
    }

    return {};
}

enum class ControlTarget {
    SERVICE,    // function gets called for the named service
    INTERFACE,  // action gets called for every service that holds this interface
};

using ControlMessageFunction = std::function<Result<void>(Service*)>;

static const std::map<std::string, ControlMessageFunction, std::less<>>& GetControlMessageMap() {
    // clang-format off
    static const std::map<std::string, ControlMessageFunction, std::less<>> control_message_functions = {
        {"sigstop_on",        [](auto* service) { service->set_sigstop(true); return Result<void>{}; }},
        {"sigstop_off",       [](auto* service) { service->set_sigstop(false); return Result<void>{}; }},
        {"oneshot_on",        [](auto* service) { service->set_oneshot(true); return Result<void>{}; }},
        {"oneshot_off",       [](auto* service) { service->set_oneshot(false); return Result<void>{}; }},
        {"start",             DoControlStart},
        {"stop",              DoControlStop},
        {"restart",           DoControlRestart},
    };
    // clang-format on

    return control_message_functions;
}

static Result<void> HandleApexControlMessage(std::string_view action, const std::string& name,
                                             std::string_view message) {
    if (action == "load") {
        return DoLoadApex(name);
    } else if (action == "unload") {
        return DoUnloadApex(name);
    } else {
        return Error() << "Unknown control msg '" << message << "'";
    }
}

static bool HandleControlMessage(std::string_view message, const std::string& name,
                                 pid_t from_pid) {
    std::string cmdline_path = StringPrintf("proc/%d/cmdline", from_pid);
    std::string process_cmdline;
    if (ReadFileToString(cmdline_path, &process_cmdline)) {
        std::replace(process_cmdline.begin(), process_cmdline.end(), '\0', ' ');
        process_cmdline = Trim(process_cmdline);
    } else {
        process_cmdline = "unknown process";
    }

    auto action = message;
    if (ConsumePrefix(&action, "apex_")) {
        if (auto result = HandleApexControlMessage(action, name, message); !result.ok()) {
            LOG(ERROR) << "Control message: Could not ctl." << message << " for '" << name
                       << "' from pid: " << from_pid << " (" << process_cmdline
                       << "): " << result.error();
            return false;
        }
        LOG(INFO) << "Control message: Processed ctl." << message << " for '" << name
                  << "' from pid: " << from_pid << " (" << process_cmdline << ")";
        return true;
    }

    Service* service = nullptr;
    if (ConsumePrefix(&action, "interface_")) {
        service = ServiceList::GetInstance().FindInterface(name);
    } else {
        service = ServiceList::GetInstance().FindService(name);
    }

    if (service == nullptr) {
        LOG(ERROR) << "Control message: Could not find '" << name << "' for ctl." << message
                   << " from pid: " << from_pid << " (" << process_cmdline << ")";
        return false;
    }

    const auto& map = GetControlMessageMap();
    const auto it = map.find(action);
    if (it == map.end()) {
        LOG(ERROR) << "Unknown control msg '" << message << "'";
        return false;
    }
    const auto& function = it->second;

    if (auto result = function(service); !result.ok()) {
        LOG(ERROR) << "Control message: Could not ctl." << message << " for '" << name
                   << "' from pid: " << from_pid << " (" << process_cmdline
                   << "): " << result.error();
        return false;
    }

    LOG(INFO) << "Control message: Processed ctl." << message << " for '" << name
              << "' from pid: " << from_pid << " (" << process_cmdline << ")";
    return true;
}

bool QueueControlMessage(const std::string& message, const std::string& name, pid_t pid, int fd) {
    auto lock = std::lock_guard{pending_control_messages_lock};
    if (pending_control_messages.size() > 100) {
        LOG(ERROR) << "Too many pending control messages, dropped '" << message << "' for '" << name
                   << "' from pid: " << pid;
        return false;
    }
    pending_control_messages.push({message, name, pid, fd});
    WakeMainInitThread();
    return true;
}

static void HandleControlMessages() {
    auto lock = std::unique_lock{pending_control_messages_lock};
    // Init historically would only execute handle one property message, including control messages
    // in each iteration of its main loop.  We retain this behavior here to prevent starvation of
    // other actions in the main loop.
    if (!pending_control_messages.empty()) {
        auto control_message = pending_control_messages.front();
        pending_control_messages.pop();
        lock.unlock();

        bool success = HandleControlMessage(control_message.message, control_message.name,
                                            control_message.pid);

        uint32_t response = success ? PROP_SUCCESS : PROP_ERROR_HANDLE_CONTROL_MESSAGE;
        if (control_message.fd != -1) {
            TEMP_FAILURE_RETRY(send(control_message.fd, &response, sizeof(response), 0));
            close(control_message.fd);
        }
        lock.lock();
    }
    // If we still have items to process, make sure we wake back up to do so.
    if (!pending_control_messages.empty()) {
        WakeMainInitThread();
    }
}

static Result<void> wait_for_coldboot_done_action(const BuiltinArguments& args) {
    if (!prop_waiter_state.StartWaiting(kColdBootDoneProp, "true")) {
        LOG(FATAL) << "Could not wait for '" << kColdBootDoneProp << "'";
    }

    return {};
}

static Result<void> SetupCgroupsAction(const BuiltinArguments&) {
    if (!CgroupsAvailable()) {
        LOG(INFO) << "Cgroups support in kernel is not enabled";
        return {};
    }
    if (!CgroupSetup()) {
        return ErrnoError() << "Failed to setup cgroups";
    }

    return {};
}

static void export_oem_lock_status() {
    if (!android::base::GetBoolProperty("ro.oem_unlock_supported", false)) {
        return;
    }
    SetProperty(
            "ro.boot.flash.locked",
            android::base::GetProperty("ro.boot.verifiedbootstate", "") == "orange" ? "0" : "1");
}

static Result<void> property_enable_triggers_action(const BuiltinArguments& args) {
    /* Enable property triggers. */
    property_triggers_enabled = 1;
    return {};
}

static Result<void> queue_property_triggers_action(const BuiltinArguments& args) {
    ActionManager::GetInstance().QueueBuiltinAction(property_enable_triggers_action, "enable_property_trigger");
    ActionManager::GetInstance().QueueAllPropertyActions();
    return {};
}

// Set the UDC controller for the ConfigFS USB Gadgets.
// Read the UDC controller in use from "/sys/class/udc".
// In case of multiple UDC controllers select the first one.
static void SetUsbController() {
    static auto controller_set = false;
    if (controller_set) return;
    std::unique_ptr<DIR, decltype(&closedir)>dir(opendir("/sys/class/udc"), closedir);
    if (!dir) return;

    dirent* dp;
    while ((dp = readdir(dir.get())) != nullptr) {
        if (dp->d_name[0] == '.') continue;

        SetProperty("sys.usb.controller", dp->d_name);
        controller_set = true;
        break;
    }
}

/// Set ro.kernel.version property to contain the major.minor pair as returned
/// by uname(2).
static void SetKernelVersion() {
    struct utsname uts;
    unsigned int major, minor;

    if ((uname(&uts) != 0) || (sscanf(uts.release, "%u.%u", &major, &minor) != 2)) {
        LOG(ERROR) << "Could not parse the kernel version from uname";
        return;
    }
    SetProperty("ro.kernel.version", android::base::StringPrintf("%u.%u", major, minor));
}

static void HandleSigtermSignal(const signalfd_siginfo& siginfo) {
    if (siginfo.ssi_pid != 0) {
        // Drop any userspace SIGTERM requests.
        LOG(DEBUG) << "Ignoring SIGTERM from pid " << siginfo.ssi_pid;
        return;
    }

    HandlePowerctlMessage("shutdown,container");
}

static void HandleSignalFd(int signal) {
    signalfd_siginfo siginfo;
    const int signal_fd = signal == SIGCHLD ? Service::GetSigchldFd() : sigterm_fd;
    ssize_t bytes_read = TEMP_FAILURE_RETRY(read(signal_fd, &siginfo, sizeof(siginfo)));
    if (bytes_read != sizeof(siginfo)) {
        PLOG(ERROR) << "Failed to read siginfo from signal_fd";
        return;
    }

    switch (siginfo.ssi_signo) {
        case SIGCHLD:
            ReapAnyOutstandingChildren();
            break;
        case SIGTERM:
            HandleSigtermSignal(siginfo);
            break;
        default:
            LOG(ERROR) << "signal_fd: received unexpected signal " << siginfo.ssi_signo;
            break;
    }
}

static void UnblockSignals() {
    const struct sigaction act { .sa_handler = SIG_DFL };
    sigaction(SIGCHLD, &act, nullptr);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_UNBLOCK, &mask, nullptr) == -1) {
        PLOG(FATAL) << "failed to unblock signals for PID " << getpid();
    }
}

static Result<void> RegisterSignalFd(Epoll* epoll, int signal, int fd) {
    return epoll->RegisterHandler(
            fd, [signal]() { HandleSignalFd(signal); }, EPOLLIN | EPOLLPRI);
}

static Result<int> CreateAndRegisterSignalFd(Epoll* epoll, int signal) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signal);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        return ErrnoError() << "failed to block signal " << signal;
    }

    unique_fd signal_fd(signalfd(-1, &mask, SFD_CLOEXEC));
    if (signal_fd.get() < 0) {
        return ErrnoError() << "failed to create signalfd for signal " << signal;
    }
    OR_RETURN(RegisterSignalFd(epoll, signal, signal_fd.get()));

    return signal_fd.release();
}

static void InstallSignalFdHandler(Epoll* epoll) {
    // Applying SA_NOCLDSTOP to a defaulted SIGCHLD handler prevents the signalfd from receiving
    // SIGCHLD when a child process stops or continues (b/77867680#comment9).
    const struct sigaction act { .sa_flags = SA_NOCLDSTOP, .sa_handler = SIG_DFL };
    sigaction(SIGCHLD, &act, nullptr);

    // Register a handler to unblock signals in the child processes.
    const int result = pthread_atfork(nullptr, nullptr, &UnblockSignals);
    if (result != 0) {
        LOG(FATAL) << "Failed to register a fork handler: " << strerror(result);
    }

    Result<void> cs_result = RegisterSignalFd(epoll, SIGCHLD, Service::GetSigchldFd());
    if (!cs_result.ok()) {
        PLOG(FATAL) << cs_result.error();
    }

    if (!IsRebootCapable()) {
        Result<int> cs_result = CreateAndRegisterSignalFd(epoll, SIGTERM);
        if (!cs_result.ok()) {
            PLOG(FATAL) << cs_result.error();
        }
        sigterm_fd = cs_result.value();
    }
}

void HandleKeychord(const std::vector<int>& keycodes) {
    // Only handle keychords if adb is enabled.
    std::string adb_enabled = android::base::GetProperty("init.svc.adbd", "");
    if (adb_enabled != "running") {
        LOG(WARNING) << "Not starting service for keychord " << android::base::Join(keycodes, ' ')
                     << " because ADB is disabled";
        return;
    }

    auto found = false;
    for (const auto& service : ServiceList::GetInstance()) {
        auto svc = service.get();
        if (svc->keycodes() == keycodes) {
            found = true;
            LOG(INFO) << "Starting service '" << svc->name() << "' from keychord "
                      << android::base::Join(keycodes, ' ');
            if (auto result = svc->Start(); !result.ok()) {
                LOG(ERROR) << "Could not start service '" << svc->name() << "' from keychord "
                           << android::base::Join(keycodes, ' ') << ": " << result.error();
            }
        }
    }
    if (!found) {
        LOG(ERROR) << "Service for keychord " << android::base::Join(keycodes, ' ') << " not found";
    }
}

static void UmountDebugRamdisk() {
    if (umount("/debug_ramdisk") != 0) {
        PLOG(ERROR) << "Failed to umount /debug_ramdisk";
    }
}

static void UmountSecondStageRes() {
    if (umount(kSecondStageRes) != 0) {
        PLOG(ERROR) << "Failed to umount " << kSecondStageRes;
    }
}

static void MountExtraFilesystems() {
#define CHECKCALL(x) \
    if ((x) != 0) PLOG(FATAL) << #x " failed.";

    // /apex is used to mount APEXes
    CHECKCALL(mount("tmpfs", "/apex", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                    "mode=0755,uid=0,gid=0"));

    if (NeedsTwoMountNamespaces()) {
        // /bootstrap-apex is used to mount "bootstrap" APEXes.
        CHECKCALL(mount("tmpfs", "/bootstrap-apex", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                        "mode=0755,uid=0,gid=0"));
    }

    // /linkerconfig is used to keep generated linker configuration
    CHECKCALL(mount("tmpfs", "/linkerconfig", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                    "mode=0755,uid=0,gid=0"));
#undef CHECKCALL
}

static void RecordStageBoottimes(const boot_clock::time_point& second_stage_start_time) {
    int64_t first_stage_start_time_ns = -1;
    if (auto first_stage_start_time_str = getenv(kEnvFirstStageStartedAt);
        first_stage_start_time_str) {
        SetProperty("ro.boottime.init", first_stage_start_time_str);
        android::base::ParseInt(first_stage_start_time_str, &first_stage_start_time_ns);
    }
    unsetenv(kEnvFirstStageStartedAt);

    int64_t selinux_start_time_ns = -1;
    if (auto selinux_start_time_str = getenv(kEnvSelinuxStartedAt); selinux_start_time_str) {
        android::base::ParseInt(selinux_start_time_str, &selinux_start_time_ns);
    }
    unsetenv(kEnvSelinuxStartedAt);

    if (selinux_start_time_ns == -1) return;
    if (first_stage_start_time_ns == -1) return;

    SetProperty("ro.boottime.init.first_stage",
                std::to_string(selinux_start_time_ns - first_stage_start_time_ns));
    SetProperty("ro.boottime.init.selinux",
                std::to_string(second_stage_start_time.time_since_epoch().count() -
                               selinux_start_time_ns));
    if (auto init_module_time_str = getenv(kEnvInitModuleDurationMs); init_module_time_str) {
        SetProperty("ro.boottime.init.modules", init_module_time_str);
        unsetenv(kEnvInitModuleDurationMs);
    }
}

void SendLoadPersistentPropertiesMessage() {
    auto init_message = InitMessage{};
    init_message.set_load_persistent_properties(true);
    if (auto result = SendMessage(property_fd, init_message); !result.ok()) {
        LOG(ERROR) << "Failed to send load persistent properties message: " << result.error();
    }
}

static Result<void> ConnectEarlyStageSnapuserdAction(const BuiltinArguments& args) {
    auto pid = GetSnapuserdFirstStagePid();
    if (!pid) {
        return {};
    }

    auto info = GetSnapuserdFirstStageInfo();
    if (auto iter = std::find(info.begin(), info.end(), "socket"s); iter == info.end()) {
        // snapuserd does not support socket handoff, so exit early.
        return {};
    }

    // Socket handoff is supported.
    auto svc = ServiceList::GetInstance().FindService("snapuserd");
    if (!svc) {
        LOG(FATAL) << "Failed to find snapuserd service entry";
    }

    svc->SetShutdownCritical();
    svc->SetStartedInFirstStage(*pid);

    svc = ServiceList::GetInstance().FindService("snapuserd_proxy");
    if (!svc) {
        LOG(FATAL) << "Failed find snapuserd_proxy service entry, merge will never initiate";
    }
    if (!svc->MarkSocketPersistent("snapuserd")) {
        LOG(FATAL) << "Could not find snapuserd socket in snapuserd_proxy service entry";
    }
    if (auto result = svc->Start(); !result.ok()) {
        LOG(FATAL) << "Could not start snapuserd_proxy: " << result.error();
    }
    return {};
}

static Result<void> CheckTradeInModeStatus([[maybe_unused]] const BuiltinArguments& args) {
    RequestTradeInModeWipeIfNeeded();
    return {};
}

static void SecondStageBootMonitor(int timeout_sec) {
    auto cur_time = boot_clock::now().time_since_epoch();
    int cur_sec = std::chrono::duration_cast<std::chrono::seconds>(cur_time).count();
    int extra_sec = timeout_sec <= cur_sec? 0 : timeout_sec - cur_sec;
    auto boot_timeout = std::chrono::seconds(extra_sec);

    LOG(INFO) << "Started BootMonitorThread, expiring in "
              << timeout_sec
              << " seconds from boot-up";

    if (!WaitForProperty("sys.boot_completed", "1", boot_timeout)) {
        LOG(ERROR) << "BootMonitorThread: boot didn't complete in "
                   << timeout_sec
                   << " seconds. Trigger a panic!";

        // add a short delay for logs to be flushed out.
        std::this_thread::sleep_for(200ms);

        // trigger a kernel panic
        WriteStringToFile("c", PROC_SYSRQ);
    }
}

static void StartSecondStageBootMonitor(int timeout_sec) {
    std::thread monitor_thread(&SecondStageBootMonitor, timeout_sec);
    monitor_thread.detach();
}

int SecondStageMain(int argc, char** argv) {
    if (REBOOT_BOOTLOADER_ON_PANIC) {
        InstallRebootSignalHandlers();
    }

    // No threads should be spin up until signalfd
    // is registered. If the threads are indeed required,
    // each of these threads _should_ make sure SIGCHLD signal
    // is blocked. See b/223076262
    boot_clock::time_point start_time = boot_clock::now();

    trigger_shutdown = [](const std::string& command) { shutdown_state.TriggerShutdown(command); };

    SetStdioToDevNull(argv);
    InitKernelLogging(argv);
    LOG(INFO) << "init second stage started!";

    SelinuxSetupKernelLogging();

    // Update $PATH in the case the second stage init is newer than first stage init, where it is
    // first set.
    if (setenv("PATH", _PATH_DEFPATH, 1) != 0) {
        PLOG(FATAL) << "Could not set $PATH to '" << _PATH_DEFPATH << "' in second stage";
    }

    // Init should not crash because of a dependence on any other process, therefore we ignore
    // SIGPIPE and handle EPIPE at the call site directly.  Note that setting a signal to SIG_IGN
    // is inherited across exec, but custom signal handlers are not.  Since we do not want to
    // ignore SIGPIPE for child processes, we set a no-op function for the signal handler instead.
    {
        struct sigaction action = {.sa_flags = SA_RESTART};
        action.sa_handler = [](int) {};
        sigaction(SIGPIPE, &action, nullptr);
    }

    // Set init and its forked children's oom_adj.
    if (auto result =
                WriteFile("/proc/1/oom_score_adj", StringPrintf("%d", DEFAULT_OOM_SCORE_ADJUST));
        !result.ok()) {
        LOG(ERROR) << "Unable to write " << DEFAULT_OOM_SCORE_ADJUST
                   << " to /proc/1/oom_score_adj: " << result.error();
    }

    // Indicate that booting is in progress to background fw loaders, etc.
    close(open("/dev/.booting", O_WRONLY | O_CREAT | O_CLOEXEC, 0000));

    // See if need to load debug props to allow adb root, when the device is unlocked.
    const char* force_debuggable_env = getenv("INIT_FORCE_DEBUGGABLE");
    bool load_debug_prop = false;
    if (force_debuggable_env && AvbHandle::IsDeviceUnlocked()) {
        load_debug_prop = "true"s == force_debuggable_env;
    }
    unsetenv("INIT_FORCE_DEBUGGABLE");

    // Umount the debug ramdisk so property service doesn't read .prop files from there, when it
    // is not meant to.
    if (!load_debug_prop) {
        UmountDebugRamdisk();
    }

    PropertyInit();

    // Umount second stage resources after property service has read the .prop files.
    UmountSecondStageRes();

    // Umount the debug ramdisk after property service has read the .prop files when it means to.
    if (load_debug_prop) {
        UmountDebugRamdisk();
    }

    // Mount extra filesystems required during second stage init
    MountExtraFilesystems();

    // Now set up SELinux for second stage.
    SelabelInitialize();
    SelinuxRestoreContext();

    Epoll epoll;
    if (auto result = epoll.Open(); !result.ok()) {
        PLOG(FATAL) << result.error();
    }

    // We always reap children before responding to the other pending functions. This is to
    // prevent a race where other daemons see that a service has exited and ask init to
    // start it again via ctl.start before init has reaped it.
    epoll.SetFirstCallback(ReapAnyOutstandingChildren);

    InstallSignalFdHandler(&epoll);
    InstallInitNotifier(&epoll);
    StartPropertyService(&property_fd);

    // If boot_timeout property has been set in a debug build, start the boot monitor
    if (GetBoolProperty("ro.debuggable", false)) {
        int timeout = GetIntProperty("ro.boot.boot_timeout", 0);
        if (timeout > 0) {
            StartSecondStageBootMonitor(timeout);
        }
    }

    // Make the time that init stages started available for bootstat to log.
    RecordStageBoottimes(start_time);

    // Set libavb version for Framework-only OTA match in Treble build.
    if (const char* avb_version = getenv("INIT_AVB_VERSION"); avb_version != nullptr) {
        SetProperty("ro.boot.avb_version", avb_version);
    }
    unsetenv("INIT_AVB_VERSION");

    fs_mgr_vendor_overlay_mount_all();
    export_oem_lock_status();
    MountHandler mount_handler(&epoll);
    SetUsbController();
    SetKernelVersion();

    const BuiltinFunctionMap& function_map = GetBuiltinFunctionMap();
    Action::set_function_map(&function_map);

    if (!SetupMountNamespaces()) {
        PLOG(FATAL) << "SetupMountNamespaces failed";
    }

    InitializeSubcontext();

    ActionManager& am = ActionManager::GetInstance();
    ServiceList& sm = ServiceList::GetInstance();

    LoadBootScripts(am, sm);

    // Turning this on and letting the INFO logging be discarded adds 0.2s to
    // Nexus 9 boot time, so it's disabled by default.
    if (false) DumpState();

    // Make the GSI status available before scripts start running.
    auto is_running = android::gsi::IsGsiRunning() ? "1" : "0";
    SetProperty(gsi::kGsiBootedProp, is_running);
    auto is_installed = android::gsi::IsGsiInstalled() ? "1" : "0";
    SetProperty(gsi::kGsiInstalledProp, is_installed);
    if (android::gsi::IsGsiRunning()) {
        std::string dsu_slot;
        if (android::gsi::GetActiveDsu(&dsu_slot)) {
            SetProperty(gsi::kDsuSlotProp, dsu_slot);
        }
    }

    // This needs to happen before SetKptrRestrictAction, as we are trying to
    // open /proc/kallsyms while still being allowed to see the full addresses
    // (since init holds CAP_SYSLOG, and Linux boots with kptr_restrict=0). The
    // address visibility through the saved fd (more specifically, the backing
    // open file description) will then be remembered by the kernel for the rest
    // of its lifetime, even after we raise the kptr_restrict.
    Service::OpenAndSaveStaticKallsymsFd();

    am.QueueBuiltinAction(SetupCgroupsAction, "SetupCgroups");
    am.QueueBuiltinAction(SetKptrRestrictAction, "SetKptrRestrict");
    am.QueueBuiltinAction(TestPerfEventSelinuxAction, "TestPerfEventSelinux");
    am.QueueEventTrigger("early-init");
    am.QueueBuiltinAction(ConnectEarlyStageSnapuserdAction, "ConnectEarlyStageSnapuserd");

    // Queue an action that waits for coldboot done so we know ueventd has set up all of /dev...
    am.QueueBuiltinAction(wait_for_coldboot_done_action, "wait_for_coldboot_done");
    am.QueueBuiltinAction(CheckTradeInModeStatus, "CheckTradeInModeStatus");
    // ... so that we can start queuing up actions that require stuff from /dev.
    am.QueueBuiltinAction(SetMmapRndBitsAction, "SetMmapRndBits");
    Keychords keychords;
    am.QueueBuiltinAction(
            [&epoll, &keychords](const BuiltinArguments& args) -> Result<void> {
                for (const auto& svc : ServiceList::GetInstance()) {
                    keychords.Register(svc->keycodes());
                }
                keychords.Start(&epoll, HandleKeychord);
                return {};
            },
            "KeychordInit");

    // Trigger all the boot actions to get us started.
    am.QueueEventTrigger("init");

    // Don't mount filesystems or start core system services in charger mode.
    std::string bootmode = GetProperty("ro.bootmode", "");
    if (bootmode == "charger") {
        am.QueueEventTrigger("charger");
    } else {
        am.QueueEventTrigger("late-init");
    }

    // Run all property triggers based on current state of the properties.
    am.QueueBuiltinAction(queue_property_triggers_action, "queue_property_triggers");

    // Restore prio before main loop
    setpriority(PRIO_PROCESS, 0, 0);
    while (true) {
        // By default, sleep until something happens. Do not convert far_future into
        // std::chrono::milliseconds because that would trigger an overflow. The unit of boot_clock
        // is 1ns.
        const boot_clock::time_point far_future = boot_clock::time_point::max();
        boot_clock::time_point next_action_time = far_future;

        auto shutdown_command = shutdown_state.CheckShutdown();
        if (shutdown_command) {
            LOG(INFO) << "Got shutdown_command '" << *shutdown_command
                      << "' Calling HandlePowerctlMessage()";
            HandlePowerctlMessage(*shutdown_command);
        }

        if (!(prop_waiter_state.MightBeWaiting() || Service::is_exec_service_running())) {
            am.ExecuteOneCommand();
            // If there's more work to do, wake up again immediately.
            if (am.HasMoreCommands()) {
                next_action_time = boot_clock::now();
            }
        }
        // Since the above code examined pending actions, no new actions must be
        // queued by the code between this line and the Epoll::Wait() call below
        // without calling WakeMainInitThread().
        if (!IsShuttingDown()) {
            auto next_process_action_time = HandleProcessActions();

            // If there's a process that needs restarting, wake up in time for that.
            if (next_process_action_time) {
                next_action_time = std::min(next_action_time, *next_process_action_time);
            }
        }

        std::optional<std::chrono::milliseconds> epoll_timeout;
        if (next_action_time != far_future) {
            epoll_timeout = std::chrono::ceil<std::chrono::milliseconds>(
                    std::max(next_action_time - boot_clock::now(), 0ns));
        }
        auto epoll_result = epoll.Wait(epoll_timeout);
        if (!epoll_result.ok()) {
            LOG(ERROR) << epoll_result.error();
        }
        if (!IsShuttingDown()) {
            HandleControlMessages();
            SetUsbController();
        }
    }

    return 0;
}

}  // namespace init
}  // namespace android
