// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <libsnapshot/snapshot.h>

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/unistd.h>
#include <sys/xattr.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>
#include <ext4_utils/ext4_utils.h>
#include <fs_mgr.h>
#include <fs_mgr/file_wait.h>
#include <fs_mgr_dm_linear.h>
#include <fstab/fstab.h>
#include <libdm/dm.h>
#include <libfiemap/image_manager.h>
#include <liblp/liblp.h>

#include <android/snapshot/snapshot.pb.h>
#include <libsnapshot/snapshot_stats.h>
#include "device_info.h"
#include "partition_cow_creator.h"
#include "scratch_super.h"
#include "snapshot_metadata_updater.h"
#include "utility.h"

namespace android {
namespace snapshot {

using aidl::android::hardware::boot::MergeStatus;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetLinear;
using android::dm::DmTargetSnapshot;
using android::dm::DmTargetUser;
using android::dm::kSectorSize;
using android::dm::SnapshotStorageMode;
using android::fiemap::FiemapStatus;
using android::fiemap::IImageManager;
using android::fs_mgr::CreateDmTable;
using android::fs_mgr::CreateLogicalPartition;
using android::fs_mgr::CreateLogicalPartitionParams;
using android::fs_mgr::GetPartitionGroupName;
using android::fs_mgr::GetPartitionName;
using android::fs_mgr::LpMetadata;
using android::fs_mgr::MetadataBuilder;
using android::fs_mgr::SlotNumberForSlotSuffix;
using chromeos_update_engine::DeltaArchiveManifest;
using chromeos_update_engine::Extent;
using chromeos_update_engine::FileDescriptor;
using chromeos_update_engine::PartitionUpdate;
template <typename T>
using RepeatedPtrField = google::protobuf::RepeatedPtrField<T>;
using std::chrono::duration_cast;
using namespace std::chrono_literals;
using namespace std::string_literals;
using android::base::Realpath;
using android::base::StringPrintf;

static constexpr char kBootSnapshotsWithoutSlotSwitch[] =
        "/metadata/ota/snapshot-boot-without-slot-switch";
static constexpr char kBootIndicatorPath[] = "/metadata/ota/snapshot-boot";
static constexpr char kRollbackIndicatorPath[] = "/metadata/ota/rollback-indicator";
static constexpr char kSnapuserdFromSystem[] = "/metadata/ota/snapuserd-from-system";
static constexpr auto kUpdateStateCheckInterval = 2s;
static constexpr char kOtaFileContext[] = "u:object_r:ota_metadata_file:s0";

/*
 * The readahead size is set to 32kb so that
 * there is no significant memory pressure (/proc/pressure/memory) during boot.
 * After OTA, during boot, partitions are scanned before marking slot as successful.
 * This scan will trigger readahead both on source and COW block device thereby
 * leading to Inactive(file) pages to be very high.
 *
 * A lower value may help reduce memory pressure further, however, that will
 * increase the boot time. Thus, for device which don't care about OTA boot
 * time, they could use O_DIRECT functionality wherein the I/O to the source
 * block device will be O_DIRECT.
 */
static constexpr auto kReadAheadSizeKb = 32;

// Note: IImageManager is an incomplete type in the header, so the default
// destructor doesn't work.
SnapshotManager::~SnapshotManager() {}

std::unique_ptr<SnapshotManager> SnapshotManager::New(IDeviceInfo* info) {
    if (!info) {
        info = new DeviceInfo();
    }

    auto sm = std::unique_ptr<SnapshotManager>(new SnapshotManager(info));
    if (info->IsTempMetadata()) {
        LOG(INFO) << "Using temp metadata from super";
    }
    return sm;
}

std::unique_ptr<SnapshotManager> SnapshotManager::NewForFirstStageMount(IDeviceInfo* info) {
    if (!info) {
        DeviceInfo* impl = new DeviceInfo();
        impl->set_first_stage_init(true);
        info = impl;
    }
    auto sm = New(info);

    // The first-stage version of snapuserd is explicitly started by init. Do
    // not attempt to using it during tests (which run in normal AOSP).
    if (!sm->device()->IsTestDevice()) {
        sm->use_first_stage_snapuserd_ = true;
    }
    return sm;
}

SnapshotManager::SnapshotManager(IDeviceInfo* device)
    : dm_(device->GetDeviceMapper()), device_(device), metadata_dir_(device_->GetMetadataDir()) {}

static std::string GetCowName(const std::string& snapshot_name) {
    return snapshot_name + "-cow";
}

SnapshotManager::SnapshotDriver SnapshotManager::GetSnapshotDriver(LockedFile* lock) {
    if (UpdateUsesUserSnapshots(lock)) {
        return SnapshotManager::SnapshotDriver::DM_USER;
    } else {
        return SnapshotManager::SnapshotDriver::DM_SNAPSHOT;
    }
}

static std::string GetDmUserCowName(const std::string& snapshot_name,
                                    SnapshotManager::SnapshotDriver driver) {
    // dm-user block device will act as a snapshot device. We identify it with
    // the same partition name so that when partitions can be mounted off
    // dm-user.

    switch (driver) {
        case SnapshotManager::SnapshotDriver::DM_USER: {
            return snapshot_name;
        }

        case SnapshotManager::SnapshotDriver::DM_SNAPSHOT: {
            return snapshot_name + "-user-cow";
        }

        default: {
            LOG(ERROR) << "Invalid snapshot driver";
            return "";
        }
    }
}

static std::string GetCowImageDeviceName(const std::string& snapshot_name) {
    return snapshot_name + "-cow-img";
}

static std::string GetBaseDeviceName(const std::string& partition_name) {
    return partition_name + "-base";
}

static std::string GetSourceDeviceName(const std::string& partition_name) {
    return partition_name + "-src";
}

bool SnapshotManager::BeginUpdate() {
    switch (TryCancelUpdate()) {
        case CancelResult::OK:
            break;
        case CancelResult::NEEDS_MERGE: {
            LOG(INFO) << "Wait for merge (if any) before beginning a new update.";
            auto state = ProcessUpdateState();
            LOG(INFO) << "Merged with end state: " << state;
            break;
        }
        default:
            LOG(ERROR) << "Cannot begin update, existing update cannot be cancelled.";
            return false;
    }

    auto file = LockExclusive();
    if (!file) return false;

    // Purge the ImageManager just in case there is a corrupt lp_metadata file
    // lying around. (NB: no need to return false on an error, we can let the
    // update try to progress.)
    if (EnsureImageManager()) {
        images_->RemoveAllImages();
    }

    // Clear any cached metadata (this allows re-using one manager across tests).
    old_partition_metadata_ = nullptr;

    auto state = ReadUpdateState(file.get());
    if (state != UpdateState::None) {
        LOG(ERROR) << "An update is already in progress, cannot begin a new update";
        return false;
    }
    return WriteUpdateState(file.get(), UpdateState::Initiated);
}

bool SnapshotManager::CancelUpdate() {
    return TryCancelUpdate() == CancelResult::OK;
}

CancelResult SnapshotManager::TryCancelUpdate() {
    auto lock = LockExclusive();
    if (!lock) return CancelResult::ERROR;

    UpdateState state = ReadUpdateState(lock.get());
    CancelResult result = IsCancelUpdateSafe(state);

    if (result != CancelResult::OK && device_->IsRecovery()) {
        LOG(ERROR) << "Cancel result " << result << " will be overridden in recovery.";
        result = CancelResult::OK;
    }

    switch (result) {
        case CancelResult::OK:
            LOG(INFO) << "Cancelling update from state: " << state;
            RemoveAllUpdateState(lock.get());
            RemoveInvalidSnapshots(lock.get());
            break;
        case CancelResult::NEEDS_MERGE:
            LOG(ERROR) << "Cannot cancel an update while a merge is in progress.";
            break;
        case CancelResult::LIVE_SNAPSHOTS:
            LOG(ERROR) << "Cannot cancel an update while snapshots are live.";
            break;
        case CancelResult::ERROR:
            // Error was already reported.
            break;
    }
    return result;
}

bool SnapshotManager::IsCancelUpdateSafe() {
    // This may be called in recovery, so ensure we have /metadata.
    auto mount = EnsureMetadataMounted();
    if (!mount || !mount->HasDevice()) {
        return true;
    }

    auto lock = LockExclusive();
    if (!lock) {
        return false;
    }

    UpdateState state = ReadUpdateState(lock.get());
    return IsCancelUpdateSafe(state) == CancelResult::OK;
}

CancelResult SnapshotManager::IsCancelUpdateSafe(UpdateState state) {
    if (IsSnapshotWithoutSlotSwitch()) {
        return CancelResult::LIVE_SNAPSHOTS;
    }

    switch (state) {
        case UpdateState::Merging:
        case UpdateState::MergeNeedsReboot:
        case UpdateState::MergeFailed:
            return CancelResult::NEEDS_MERGE;
        case UpdateState::Unverified: {
            // We completed an update, but it can still be canceled if we haven't booted into it.
            auto slot = GetCurrentSlot();
            if (slot == Slot::Target) {
                return CancelResult::LIVE_SNAPSHOTS;
            }
            return CancelResult::OK;
        }
        case UpdateState::None:
        case UpdateState::Initiated:
        case UpdateState::Cancelled:
            return CancelResult::OK;
        default:
            LOG(ERROR) << "Unknown state: " << state;
            return CancelResult::ERROR;
    }
}

std::string SnapshotManager::ReadUpdateSourceSlotSuffix() {
    auto boot_file = GetSnapshotBootIndicatorPath();
    std::string contents;
    if (!android::base::ReadFileToString(boot_file, &contents)) {
        return {};
    }
    return contents;
}

SnapshotManager::Slot SnapshotManager::GetCurrentSlot() {
    auto contents = ReadUpdateSourceSlotSuffix();
    if (contents.empty()) {
        return Slot::Unknown;
    }
    if (device_->GetSlotSuffix() == contents) {
        return Slot::Source;
    }
    return Slot::Target;
}

std::string SnapshotManager::GetSnapshotSlotSuffix() {
    switch (GetCurrentSlot()) {
        case Slot::Target:
            return device_->GetSlotSuffix();
        default:
            return device_->GetOtherSlotSuffix();
    }
}

static bool RemoveFileIfExists(const std::string& path) {
    std::string message;
    if (!android::base::RemoveFileIfExists(path, &message)) {
        LOG(ERROR) << "Remove failed: " << path << ": " << message;
        return false;
    }
    return true;
}

bool SnapshotManager::RemoveAllUpdateState(LockedFile* lock, const std::function<bool()>& prolog) {
    if (prolog && !prolog()) {
        LOG(WARNING) << "Can't RemoveAllUpdateState: prolog failed.";
        return false;
    }

    LOG(INFO) << "Removing all update state.";

    if (ReadUpdateState(lock) != UpdateState::None) {
        // Only call this if we're actually cancelling an update. It's not
        // expected to yield anything otherwise, and firing up gsid on normal
        // boot is expensive.
        if (!RemoveAllSnapshots(lock)) {
            LOG(ERROR) << "Could not remove all snapshots";
            return false;
        }
    }

    // It's okay if these fail:
    // - For SnapshotBoot and Rollback, first-stage init performs a deeper check after
    // reading the indicator file, so it's not a problem if it still exists
    // after the update completes.
    // - For ForwardMerge, FinishedSnapshotWrites asserts that the existence of the indicator
    // matches the incoming update.
    std::vector<std::string> files = {
            GetSnapshotBootIndicatorPath(),          GetRollbackIndicatorPath(),
            GetForwardMergeIndicatorPath(),          GetOldPartitionMetadataPath(),
            GetBootSnapshotsWithoutSlotSwitchPath(), GetSnapuserdFromSystemPath(),
    };
    for (const auto& file : files) {
        RemoveFileIfExists(file);
    }

    // If this fails, we'll keep trying to remove the update state (as the
    // device reboots or starts a new update) until it finally succeeds.
    return WriteUpdateState(lock, UpdateState::None);
}

bool SnapshotManager::FinishedSnapshotWrites(bool wipe) {
    auto lock = LockExclusive();
    if (!lock) return false;

    auto update_state = ReadUpdateState(lock.get());
    if (update_state == UpdateState::Unverified) {
        LOG(INFO) << "FinishedSnapshotWrites already called before. Ignored.";
        return true;
    }

    if (update_state != UpdateState::Initiated) {
        LOG(ERROR) << "Can only transition to the Unverified state from the Initiated state.";
        return false;
    }

    if (!EnsureNoOverflowSnapshot(lock.get())) {
        LOG(ERROR) << "Cannot ensure there are no overflow snapshots.";
        return false;
    }

    if (!UpdateForwardMergeIndicator(wipe)) {
        return false;
    }

    // This file is written on boot to detect whether a rollback occurred. It
    // MUST NOT exist before rebooting, otherwise, we're at risk of deleting
    // snapshots too early.
    if (!RemoveFileIfExists(GetRollbackIndicatorPath())) {
        return false;
    }

    // This file acts as both a quick indicator for init (it can use access(2)
    // to decide how to do first-stage mounts), and it stores the old slot, so
    // we can tell whether or not we performed a rollback.
    auto contents = device_->GetSlotSuffix();
    auto boot_file = GetSnapshotBootIndicatorPath();
    if (!WriteStringToFileAtomic(contents, boot_file)) {
        PLOG(ERROR) << "write failed: " << boot_file;
        return false;
    }
    return WriteUpdateState(lock.get(), UpdateState::Unverified);
}

bool SnapshotManager::CreateSnapshot(LockedFile* lock, PartitionCowCreator* cow_creator,
                                     SnapshotStatus* status) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    CHECK(status);

    if (status->name().empty()) {
        LOG(ERROR) << "SnapshotStatus has no name.";
        return false;
    }
    // Check these sizes. Like liblp, we guarantee the partition size is
    // respected, which means it has to be sector-aligned. (This guarantee is
    // useful for locating avb footers correctly). The COW file size, however,
    // can be arbitrarily larger than specified, so we can safely round it up.
    if (status->device_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << status->name()
                   << " device size is not a multiple of the sector size: "
                   << status->device_size();
        return false;
    }
    if (status->snapshot_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << status->name()
                   << " snapshot size is not a multiple of the sector size: "
                   << status->snapshot_size();
        return false;
    }
    if (status->cow_partition_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << status->name()
                   << " cow partition size is not a multiple of the sector size: "
                   << status->cow_partition_size();
        return false;
    }
    if (status->cow_file_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << status->name()
                   << " cow file size is not a multiple of the sector size: "
                   << status->cow_file_size();
        return false;
    }

    status->set_state(SnapshotState::CREATED);
    status->set_sectors_allocated(0);
    status->set_metadata_sectors(0);
    status->set_using_snapuserd(cow_creator->using_snapuserd);
    status->set_compression_algorithm(cow_creator->compression_algorithm);
    status->set_compression_factor(cow_creator->compression_factor);
    status->set_read_ahead_size(cow_creator->read_ahead_size);
    if (cow_creator->enable_threading) {
        status->set_enable_threading(cow_creator->enable_threading);
    }
    if (cow_creator->batched_writes) {
        status->set_batched_writes(cow_creator->batched_writes);
    }

    if (!WriteSnapshotStatus(lock, *status)) {
        PLOG(ERROR) << "Could not write snapshot status: " << status->name();
        return false;
    }
    return true;
}

Return SnapshotManager::CreateCowImage(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    if (!EnsureImageManager()) return Return::Error();

    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return Return::Error();
    }

    // The COW file size should have been rounded up to the nearest sector in CreateSnapshot.
    if (status.cow_file_size() % kSectorSize != 0) {
        LOG(ERROR) << "Snapshot " << name << " COW file size is not a multiple of the sector size: "
                   << status.cow_file_size();
        return Return::Error();
    }

    std::string cow_image_name = GetCowImageDeviceName(name);
    int cow_flags = IImageManager::CREATE_IMAGE_DEFAULT;
    return Return(images_->CreateBackingImage(cow_image_name, status.cow_file_size(), cow_flags));
}

bool SnapshotManager::MapDmUserCow(LockedFile* lock, const std::string& name,
                                   const std::string& cow_file, const std::string& base_device,
                                   const std::string& base_path_merge,
                                   const std::chrono::milliseconds& timeout_ms, std::string* path) {
    CHECK(lock);

    if (UpdateUsesUserSnapshots(lock)) {
        SnapshotStatus status;
        if (!ReadSnapshotStatus(lock, name, &status)) {
            LOG(ERROR) << "MapDmUserCow: ReadSnapshotStatus failed...";
            return false;
        }

        if (status.state() == SnapshotState::NONE ||
            status.state() == SnapshotState::MERGE_COMPLETED) {
            LOG(ERROR) << "Should not create a snapshot device for " << name
                       << " after merging has completed.";
            return false;
        }

        SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
        if (update_status.state() == UpdateState::MergeCompleted ||
            update_status.state() == UpdateState::MergeNeedsReboot) {
            LOG(ERROR) << "Should not create a snapshot device for " << name
                       << " after global merging has completed.";
            return false;
        }
    }

    // Use an extra decoration for first-stage init, so we can transition
    // to a new table entry in second-stage.
    std::string misc_name = name;
    if (use_first_stage_snapuserd_) {
        misc_name += "-init";
    }

    if (!EnsureSnapuserdConnected()) {
        return false;
    }

    uint64_t base_sectors = 0;
    if (!UpdateUsesUserSnapshots(lock)) {
        base_sectors = snapuserd_client_->InitDmUserCow(misc_name, cow_file, base_device);
        if (base_sectors == 0) {
            LOG(ERROR) << "Failed to retrieve base_sectors from Snapuserd";
            return false;
        }
    } else if (IsSnapshotWithoutSlotSwitch()) {
        // When snapshots are on current slot, we determine the size
        // of block device based on the number of COW operations. We cannot
        // use base device as it will be from older image.
        unique_fd fd(open(cow_file.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd < 0) {
            PLOG(ERROR) << "Failed to open " << cow_file;
            return false;
        }

        CowReader reader;
        if (!reader.Parse(std::move(fd))) {
            LOG(ERROR) << "Failed to parse cow " << cow_file;
            return false;
        }

        uint64_t dev_sz = 0;
        const auto& header = reader.GetHeader();
        if (header.prefix.major_version == 2) {
            const size_t num_ops = reader.get_num_total_data_ops();
            dev_sz = (num_ops * header.block_size);
        } else {
            // create_snapshot will skip in-place copy ops. Hence, fetch this
            // information directly from v3 header.
            const auto& v3_header = reader.header_v3();
            dev_sz = v3_header.op_count_max * v3_header.block_size;
        }

        base_sectors = dev_sz >> 9;
    } else {
        // For userspace snapshots, the size of the base device is taken as the
        // size of the dm-user block device. Since there is no pseudo mapping
        // created in the daemon, we no longer need to rely on the daemon for
        // sizing the dm-user block device.
        unique_fd fd(TEMP_FAILURE_RETRY(open(base_path_merge.c_str(), O_RDONLY | O_CLOEXEC)));
        if (fd < 0) {
            LOG(ERROR) << "Cannot open block device: " << base_path_merge;
            return false;
        }

        uint64_t dev_sz = get_block_device_size(fd.get());
        if (!dev_sz) {
            LOG(ERROR) << "Failed to find block device size: " << base_path_merge;
            return false;
        }

        base_sectors = dev_sz >> 9;
    }

    DmTable table;
    table.Emplace<DmTargetUser>(0, base_sectors, misc_name);
    if (!dm_.CreateDevice(name, table, path, timeout_ms)) {
        LOG(ERROR) << " dm-user: CreateDevice failed... ";
        return false;
    }
    if (!WaitForDevice(*path, timeout_ms)) {
        LOG(ERROR) << " dm-user: timeout: Failed to create block device for: " << name;
        return false;
    }

    auto control_device = "/dev/dm-user/" + misc_name;
    if (!WaitForDevice(control_device, timeout_ms)) {
        return false;
    }

    if (UpdateUsesUserSnapshots(lock)) {
        // Now that the dm-user device is created, initialize the daemon and
        // spin up the worker threads.
        if (!snapuserd_client_->InitDmUserCow(misc_name, cow_file, base_device, base_path_merge)) {
            LOG(ERROR) << "InitDmUserCow failed";
            return false;
        }
    }

    return snapuserd_client_->AttachDmUser(misc_name);
}

bool SnapshotManager::MapSnapshot(LockedFile* lock, const std::string& name,
                                  const std::string& base_device, const std::string& cow_device,
                                  const std::chrono::milliseconds& timeout_ms,
                                  std::string* dev_path) {
    CHECK(lock);

    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return false;
    }
    if (status.state() == SnapshotState::NONE || status.state() == SnapshotState::MERGE_COMPLETED) {
        LOG(ERROR) << "Should not create a snapshot device for " << name
                   << " after merging has completed.";
        return false;
    }

    // Validate the block device size, as well as the requested snapshot size.
    // Note that during first-stage init, we don't have the device paths.
    if (android::base::StartsWith(base_device, "/")) {
        unique_fd fd(open(base_device.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd < 0) {
            PLOG(ERROR) << "open failed: " << base_device;
            return false;
        }
        auto dev_size = get_block_device_size(fd);
        if (!dev_size) {
            PLOG(ERROR) << "Could not determine block device size: " << base_device;
            return false;
        }
        if (status.device_size() != dev_size) {
            LOG(ERROR) << "Block device size for " << base_device << " does not match"
                       << "(expected " << status.device_size() << ", got " << dev_size << ")";
            return false;
        }
    }
    if (status.device_size() % kSectorSize != 0) {
        LOG(ERROR) << "invalid blockdev size for " << base_device << ": " << status.device_size();
        return false;
    }
    if (status.snapshot_size() % kSectorSize != 0 ||
        status.snapshot_size() > status.device_size()) {
        LOG(ERROR) << "Invalid snapshot size for " << base_device << ": " << status.snapshot_size();
        return false;
    }
    if (status.device_size() != status.snapshot_size()) {
        LOG(ERROR) << "Device size and snapshot size must be the same (device size = "
                   << status.device_size() << ", snapshot size = " << status.snapshot_size();
        return false;
    }

    uint64_t snapshot_sectors = status.snapshot_size() / kSectorSize;

    // Note that merging is a global state. We do track whether individual devices
    // have completed merging, but the start of the merge process is considered
    // atomic.
    SnapshotStorageMode mode;
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    switch (update_status.state()) {
        case UpdateState::MergeCompleted:
        case UpdateState::MergeNeedsReboot:
            LOG(ERROR) << "Should not create a snapshot device for " << name
                       << " after global merging has completed.";
            return false;
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            // Note: MergeFailed indicates that a merge is in progress, but
            // is possibly stalled. We still have to honor the merge.
            if (DecideMergePhase(status) == update_status.merge_phase()) {
                mode = SnapshotStorageMode::Merge;
            } else {
                mode = SnapshotStorageMode::Persistent;
            }
            break;
        default:
            mode = SnapshotStorageMode::Persistent;
            break;
    }

    if (mode == SnapshotStorageMode::Persistent && status.state() == SnapshotState::MERGING) {
        LOG(ERROR) << "Snapshot: " << name
                   << " has snapshot status Merging but mode set to Persistent."
                   << " Changing mode to Snapshot-Merge.";
        mode = SnapshotStorageMode::Merge;
    }

    DmTable table;
    table.Emplace<DmTargetSnapshot>(0, snapshot_sectors, base_device, cow_device, mode,
                                    kSnapshotChunkSize);
    if (!dm_.CreateDevice(name, table, dev_path, timeout_ms)) {
        LOG(ERROR) << "Could not create snapshot device: " << name;
        return false;
    }
    return true;
}

std::optional<std::string> SnapshotManager::MapCowImage(
        const std::string& name, const std::chrono::milliseconds& timeout_ms) {
    if (!EnsureImageManager()) return std::nullopt;
    auto cow_image_name = GetCowImageDeviceName(name);

    bool ok;
    std::string cow_dev;
    if (device_->IsRecovery() || device_->IsFirstStageInit()) {
        const auto& opener = device_->GetPartitionOpener();
        ok = images_->MapImageWithDeviceMapper(opener, cow_image_name, &cow_dev);
    } else {
        ok = images_->MapImageDevice(cow_image_name, timeout_ms, &cow_dev);
    }

    if (ok) {
        LOG(INFO) << "Mapped " << cow_image_name << " to " << cow_dev;
        return cow_dev;
    }
    LOG(ERROR) << "Could not map image device: " << cow_image_name;
    return std::nullopt;
}

bool SnapshotManager::MapSourceDevice(LockedFile* lock, const std::string& name,
                                      const std::chrono::milliseconds& timeout_ms,
                                      std::string* path) {
    CHECK(lock);

    auto metadata = ReadOldPartitionMetadata(lock);
    if (!metadata) {
        LOG(ERROR) << "Could not map source device due to missing or corrupt metadata";
        return false;
    }

    auto old_name = GetOtherPartitionName(name);
    auto slot_suffix = device_->GetSlotSuffix();
    auto slot = SlotNumberForSlotSuffix(slot_suffix);

    CreateLogicalPartitionParams params = {
            .block_device = device_->GetSuperDevice(slot),
            .metadata = metadata,
            .partition_name = old_name,
            .timeout_ms = timeout_ms,
            .device_name = GetSourceDeviceName(name),
            .partition_opener = &device_->GetPartitionOpener(),
    };
    if (!CreateLogicalPartition(std::move(params), path)) {
        LOG(ERROR) << "Could not create source device for snapshot " << name;
        return false;
    }
    return true;
}

bool SnapshotManager::UnmapSnapshot(LockedFile* lock, const std::string& name) {
    CHECK(lock);

    if (UpdateUsesUserSnapshots(lock)) {
        if (!UnmapUserspaceSnapshotDevice(lock, name)) {
            return false;
        }
    } else {
        if (!DeleteDeviceIfExists(name)) {
            LOG(ERROR) << "Could not delete snapshot device: " << name;
            return false;
        }
    }
    return true;
}

bool SnapshotManager::UnmapCowImage(const std::string& name) {
    if (!EnsureImageManager()) return false;
    return images_->UnmapImageIfExists(GetCowImageDeviceName(name));
}

bool SnapshotManager::DeleteSnapshot(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    if (!EnsureImageManager()) return false;

    if (!UnmapCowDevices(lock, name)) {
        return false;
    }

    // We can't delete snapshots in recovery. The only way we'd try is it we're
    // completing or canceling a merge in preparation for a data wipe, in which
    // case, we don't care if the file sticks around.
    if (device_->IsRecovery()) {
        LOG(INFO) << "Skipping delete of snapshot " << name << " in recovery.";
        return true;
    }

    auto cow_image_name = GetCowImageDeviceName(name);
    if (images_->BackingImageExists(cow_image_name)) {
        if (!images_->DeleteBackingImage(cow_image_name)) {
            return false;
        }
    }

    std::string error;
    auto file_path = GetSnapshotStatusFilePath(name);
    if (!android::base::RemoveFileIfExists(file_path, &error)) {
        LOG(ERROR) << "Failed to remove status file " << file_path << ": " << error;
        return false;
    }

    // This path may never exist. If it is present, then it's a stale
    // snapshot status file. Just remove the file and log the message.
    const std::string tmp_path = file_path + ".tmp";
    if (!android::base::RemoveFileIfExists(tmp_path, &error)) {
        LOG(ERROR) << "Failed to remove stale snapshot file " << tmp_path;
    }

    return true;
}

bool SnapshotManager::InitiateMerge() {
    auto lock = LockExclusive();
    if (!lock) return false;

    UpdateState state = ReadUpdateState(lock.get());
    if (state != UpdateState::Unverified) {
        LOG(ERROR) << "Cannot begin a merge if an update has not been verified";
        return false;
    }

    auto slot = GetCurrentSlot();
    if (slot != Slot::Target) {
        LOG(ERROR) << "Device cannot merge while not booting from new slot";
        return false;
    }

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        return false;
    }

    auto current_slot_suffix = device_->GetSlotSuffix();

    for (const auto& snapshot : snapshots) {
        if (!android::base::EndsWith(snapshot, current_slot_suffix)) {
            // Allow the merge to continue, but log this unexpected case.
            LOG(ERROR) << "Unexpected snapshot found during merge: " << snapshot;
            continue;
        }

        // The device has to be mapped, since everything should be merged at
        // the same time. This is a fairly serious error. We could forcefully
        // map everything here, but it should have been mapped during first-
        // stage init.
        if (dm_.GetState(snapshot) == DmDeviceState::INVALID) {
            LOG(ERROR) << "Cannot begin merge; device " << snapshot << " is not mapped.";
            return false;
        }
    }

    auto metadata = ReadCurrentMetadata();
    for (auto it = snapshots.begin(); it != snapshots.end();) {
        switch (GetMetadataPartitionState(*metadata, *it)) {
            case MetadataPartitionState::Flashed:
                LOG(WARNING) << "Detected re-flashing for partition " << *it
                             << ". Skip merging it.";
                [[fallthrough]];
            case MetadataPartitionState::None: {
                LOG(WARNING) << "Deleting snapshot for partition " << *it;
                if (!DeleteSnapshot(lock.get(), *it)) {
                    LOG(WARNING) << "Cannot delete snapshot for partition " << *it
                                 << ". Skip merging it anyways.";
                }
                it = snapshots.erase(it);
            } break;
            case MetadataPartitionState::Updated: {
                ++it;
            } break;
        }
    }

    bool using_snapuserd = false;

    std::vector<std::string> first_merge_group;

    DmTargetSnapshot::Status initial_target_values = {};
    for (const auto& snapshot : snapshots) {
        if (!UpdateUsesUserSnapshots(lock.get())) {
            DmTargetSnapshot::Status current_status;
            if (!QuerySnapshotStatus(snapshot, nullptr, &current_status)) {
                return false;
            }
            initial_target_values.sectors_allocated += current_status.sectors_allocated;
            initial_target_values.total_sectors += current_status.total_sectors;
            initial_target_values.metadata_sectors += current_status.metadata_sectors;
        }

        SnapshotStatus snapshot_status;
        if (!ReadSnapshotStatus(lock.get(), snapshot, &snapshot_status)) {
            return false;
        }

        using_snapuserd |= snapshot_status.using_snapuserd();
        if (DecideMergePhase(snapshot_status) == MergePhase::FIRST_PHASE) {
            first_merge_group.emplace_back(snapshot);
        }
    }

    SnapshotUpdateStatus initial_status = ReadSnapshotUpdateStatus(lock.get());
    initial_status.set_state(UpdateState::Merging);
    initial_status.set_using_snapuserd(using_snapuserd);

    if (!UpdateUsesUserSnapshots(lock.get())) {
        initial_status.set_sectors_allocated(initial_target_values.sectors_allocated);
        initial_status.set_total_sectors(initial_target_values.total_sectors);
        initial_status.set_metadata_sectors(initial_target_values.metadata_sectors);
    }

    // If any partitions shrunk, we need to merge them before we merge any other
    // partitions (see b/177935716). Otherwise, a merge from another partition
    // may overwrite the source block of a copy operation.
    const std::vector<std::string>* merge_group;
    if (first_merge_group.empty()) {
        merge_group = &snapshots;
        initial_status.set_merge_phase(MergePhase::SECOND_PHASE);
    } else {
        merge_group = &first_merge_group;
        initial_status.set_merge_phase(MergePhase::FIRST_PHASE);
    }

    // Point of no return - mark that we're starting a merge. From now on every
    // eligible snapshot must be a merge target.
    if (!WriteSnapshotUpdateStatus(lock.get(), initial_status)) {
        return false;
    }

    auto reported_code = MergeFailureCode::Ok;
    for (const auto& snapshot : *merge_group) {
        // If this fails, we have no choice but to continue. Everything must
        // be merged. This is not an ideal state to be in, but it is safe,
        // because we the next boot will try again.
        auto code = SwitchSnapshotToMerge(lock.get(), snapshot);
        if (code != MergeFailureCode::Ok) {
            LOG(ERROR) << "Failed to switch snapshot to a merge target: " << snapshot;
            if (reported_code == MergeFailureCode::Ok) {
                reported_code = code;
            }
        }
    }

    // If we couldn't switch everything to a merge target, pre-emptively mark
    // this merge as failed. It will get acknowledged when WaitForMerge() is
    // called.
    if (reported_code != MergeFailureCode::Ok) {
        WriteUpdateState(lock.get(), UpdateState::MergeFailed, reported_code);
    }

    // Return true no matter what, because a merge was initiated.
    return true;
}

MergeFailureCode SnapshotManager::SwitchSnapshotToMerge(LockedFile* lock, const std::string& name) {
    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock, name, &status)) {
        return MergeFailureCode::ReadStatus;
    }
    if (status.state() != SnapshotState::CREATED) {
        LOG(WARNING) << "Snapshot " << name
                     << " has unexpected state: " << SnapshotState_Name(status.state());
    }

    if (UpdateUsesUserSnapshots(lock)) {
        if (EnsureSnapuserdConnected()) {
            // This is the point where we inform the daemon to initiate/resume
            // the merge
            if (!snapuserd_client_->InitiateMerge(name)) {
                return MergeFailureCode::UnknownTable;
            }
        } else {
            LOG(ERROR) << "Failed to connect to snapuserd daemon to initiate merge";
            return MergeFailureCode::UnknownTable;
        }
    } else {
        // After this, we return true because we technically did switch to a merge
        // target. Everything else we do here is just informational.
        if (auto code = RewriteSnapshotDeviceTable(name); code != MergeFailureCode::Ok) {
            return code;
        }
    }

    status.set_state(SnapshotState::MERGING);

    if (!UpdateUsesUserSnapshots(lock)) {
        DmTargetSnapshot::Status dm_status;
        if (!QuerySnapshotStatus(name, nullptr, &dm_status)) {
            LOG(ERROR) << "Could not query merge status for snapshot: " << name;
        }
        status.set_sectors_allocated(dm_status.sectors_allocated);
        status.set_metadata_sectors(dm_status.metadata_sectors);
    }

    if (!WriteSnapshotStatus(lock, status)) {
        LOG(ERROR) << "Could not update status file for snapshot: " << name;
    }
    return MergeFailureCode::Ok;
}

MergeFailureCode SnapshotManager::RewriteSnapshotDeviceTable(const std::string& name) {
    std::vector<DeviceMapper::TargetInfo> old_targets;
    if (!dm_.GetTableInfo(name, &old_targets)) {
        LOG(ERROR) << "Could not read snapshot device table: " << name;
        return MergeFailureCode::GetTableInfo;
    }
    if (old_targets.size() != 1 || DeviceMapper::GetTargetType(old_targets[0].spec) != "snapshot") {
        LOG(ERROR) << "Unexpected device-mapper table for snapshot: " << name;
        return MergeFailureCode::UnknownTable;
    }

    std::string base_device, cow_device;
    if (!DmTargetSnapshot::GetDevicesFromParams(old_targets[0].data, &base_device, &cow_device)) {
        LOG(ERROR) << "Could not derive underlying devices for snapshot: " << name;
        return MergeFailureCode::GetTableParams;
    }

    DmTable table;
    table.Emplace<DmTargetSnapshot>(0, old_targets[0].spec.length, base_device, cow_device,
                                    SnapshotStorageMode::Merge, kSnapshotChunkSize);
    if (!dm_.LoadTableAndActivate(name, table)) {
        LOG(ERROR) << "Could not swap device-mapper tables on snapshot device " << name;
        return MergeFailureCode::ActivateNewTable;
    }
    LOG(INFO) << "Successfully switched snapshot device to a merge target: " << name;
    return MergeFailureCode::Ok;
}

bool SnapshotManager::GetSingleTarget(const std::string& dm_name, TableQuery query,
                                      DeviceMapper::TargetInfo* target) {
    if (dm_.GetState(dm_name) == DmDeviceState::INVALID) {
        return false;
    }

    std::vector<DeviceMapper::TargetInfo> targets;
    bool result;
    if (query == TableQuery::Status) {
        result = dm_.GetTableStatus(dm_name, &targets);
    } else {
        result = dm_.GetTableInfo(dm_name, &targets);
    }
    if (!result) {
        LOG(ERROR) << "Could not query device: " << dm_name;
        return false;
    }
    if (targets.size() != 1) {
        return false;
    }

    *target = std::move(targets[0]);
    return true;
}

bool SnapshotManager::IsSnapshotDevice(const std::string& dm_name, TargetInfo* target) {
    DeviceMapper::TargetInfo snap_target;
    if (!GetSingleTarget(dm_name, TableQuery::Status, &snap_target)) {
        return false;
    }
    auto type = DeviceMapper::GetTargetType(snap_target.spec);

    // If this is not a user-snapshot device then it should either
    // be a dm-snapshot or dm-snapshot-merge target
    if (type != "user") {
        if (type != "snapshot" && type != "snapshot-merge") {
            return false;
        }
    }

    if (target) {
        *target = std::move(snap_target);
    }
    return true;
}

auto SnapshotManager::UpdateStateToStr(const enum UpdateState state) {
    switch (state) {
        case None:
            return "None";
        case Initiated:
            return "Initiated";
        case Unverified:
            return "Unverified";
        case Merging:
            return "Merging";
        case MergeNeedsReboot:
            return "MergeNeedsReboot";
        case MergeCompleted:
            return "MergeCompleted";
        case MergeFailed:
            return "MergeFailed";
        case Cancelled:
            return "Cancelled";
        default:
            return "Unknown";
    }
}

bool SnapshotManager::QuerySnapshotStatus(const std::string& dm_name, std::string* target_type,
                                          DmTargetSnapshot::Status* status) {
    DeviceMapper::TargetInfo target;
    if (!IsSnapshotDevice(dm_name, &target)) {
        LOG(ERROR) << "Device " << dm_name << " is not a snapshot or snapshot-merge device";
        return false;
    }
    if (!DmTargetSnapshot::ParseStatusText(target.data, status)) {
        LOG(ERROR) << "Could not parse snapshot status text: " << dm_name;
        return false;
    }
    if (target_type) {
        *target_type = DeviceMapper::GetTargetType(target.spec);
    }
    if (!status->error.empty()) {
        LOG(ERROR) << "Snapshot: " << dm_name << " returned error code: " << status->error;
        return false;
    }
    return true;
}

// Note that when a merge fails, we will *always* try again to complete the
// merge each time the device boots. There is no harm in doing so, and if
// the problem was transient, we might manage to get a new outcome.
UpdateState SnapshotManager::ProcessUpdateState(const std::function<bool()>& callback,
                                                const std::function<bool()>& before_cancel) {
    while (true) {
        auto result = CheckMergeState(before_cancel);
        LOG(INFO) << "ProcessUpdateState handling state: " << UpdateStateToStr(result.state);

        if (result.state == UpdateState::MergeFailed) {
            AcknowledgeMergeFailure(result.failure_code);
        }

        if (result.state == UpdateState::MergeCompleted) {
            if (device_->IsTempMetadata()) {
                CleanupScratchOtaMetadataIfPresent();
            }
        }

        if (result.state != UpdateState::Merging) {
            // Either there is no merge, or the merge was finished, so no need
            // to keep waiting.
            return result.state;
        }

        if (callback && !callback()) {
            return result.state;
        }

        // This wait is not super time sensitive, so we have a relatively
        // low polling frequency.
        std::this_thread::sleep_for(kUpdateStateCheckInterval);
    }
}

auto SnapshotManager::CheckMergeState(const std::function<bool()>& before_cancel) -> MergeResult {
    auto lock = LockExclusive();
    if (!lock) {
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::AcquireLock);
    }

    auto result = CheckMergeState(lock.get(), before_cancel);
    LOG(INFO) << "CheckMergeState for snapshots returned: " << UpdateStateToStr(result.state);

    if (result.state == UpdateState::MergeCompleted) {
        // Do this inside the same lock. Failures get acknowledged without the
        // lock, because flock() might have failed.
        AcknowledgeMergeSuccess(lock.get());
    } else if (result.state == UpdateState::Cancelled) {
        if (!device_->IsRecovery() && !RemoveAllUpdateState(lock.get(), before_cancel)) {
            LOG(ERROR) << "Failed to remove all update state after acknowleding cancelled update.";
        }
    }
    return result;
}

auto SnapshotManager::CheckMergeState(LockedFile* lock,
                                      const std::function<bool()>& before_cancel) -> MergeResult {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    switch (update_status.state()) {
        case UpdateState::None:
        case UpdateState::MergeCompleted:
            // Harmless races are allowed between two callers of WaitForMerge,
            // so in both of these cases we just propagate the state.
            return MergeResult(update_status.state());

        case UpdateState::Merging:
        case UpdateState::MergeNeedsReboot:
        case UpdateState::MergeFailed:
            // We'll poll each snapshot below. Note that for the NeedsReboot
            // case, we always poll once to give cleanup another opportunity to
            // run.
            break;

        case UpdateState::Unverified:
            // This is an edge case. Normally cancelled updates are detected
            // via the merge poll below, but if we never started a merge, we
            // need to also check here.
            if (HandleCancelledUpdate(lock, before_cancel)) {
                return MergeResult(UpdateState::Cancelled);
            }
            return MergeResult(update_status.state());

        default:
            return MergeResult(update_status.state());
    }

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::ListSnapshots);
    }

    auto current_slot_suffix = device_->GetSlotSuffix();

    bool cancelled = false;
    bool merging = false;
    bool needs_reboot = false;
    bool wrong_phase = false;
    MergeFailureCode failure_code = MergeFailureCode::Ok;
    for (const auto& snapshot : snapshots) {
        if (!android::base::EndsWith(snapshot, current_slot_suffix)) {
            // This will have triggered an error message in InitiateMerge already.
            LOG(ERROR) << "Skipping merge validation of unexpected snapshot: " << snapshot;
            continue;
        }

        auto result = CheckTargetMergeState(lock, snapshot, update_status);
        LOG(INFO) << "CheckTargetMergeState for " << snapshot
                  << " returned: " << UpdateStateToStr(result.state);

        switch (result.state) {
            case UpdateState::MergeFailed:
                // Take the first failure code in case other failures compound.
                if (failure_code == MergeFailureCode::Ok) {
                    failure_code = result.failure_code;
                }
                break;
            case UpdateState::Merging:
                merging = true;
                break;
            case UpdateState::MergeNeedsReboot:
                needs_reboot = true;
                break;
            case UpdateState::MergeCompleted:
                break;
            case UpdateState::Cancelled:
                cancelled = true;
                break;
            case UpdateState::None:
                wrong_phase = true;
                break;
            default:
                LOG(ERROR) << "Unknown merge status for \"" << snapshot << "\": " << "\""
                           << result.state << "\"";
                if (failure_code == MergeFailureCode::Ok) {
                    failure_code = MergeFailureCode::UnexpectedMergeState;
                }
                break;
        }
    }

    if (merging) {
        // Note that we handle "Merging" before we handle anything else. We
        // want to poll until *nothing* is merging if we can, so everything has
        // a chance to get marked as completed or failed.
        return MergeResult(UpdateState::Merging);
    }
    if (failure_code != MergeFailureCode::Ok) {
        // Note: since there are many drop-out cases for failure, we acknowledge
        // it in WaitForMerge rather than here and elsewhere.
        return MergeResult(UpdateState::MergeFailed, failure_code);
    }
    if (wrong_phase) {
        // If we got here, no other partitions are being merged, and nothing
        // failed to merge. It's safe to move to the next merge phase.
        auto code = MergeSecondPhaseSnapshots(lock);
        if (code != MergeFailureCode::Ok) {
            return MergeResult(UpdateState::MergeFailed, code);
        }
        return MergeResult(UpdateState::Merging);
    }
    if (needs_reboot) {
        WriteUpdateState(lock, UpdateState::MergeNeedsReboot);
        return MergeResult(UpdateState::MergeNeedsReboot);
    }
    if (cancelled) {
        // This is an edge case, that we handle as correctly as we sensibly can.
        // The underlying partition has changed behind update_engine, and we've
        // removed the snapshot as a result. The exact state of the update is
        // undefined now, but this can only happen on an unlocked device where
        // partitions can be flashed without wiping userdata.
        return MergeResult(UpdateState::Cancelled);
    }
    return MergeResult(UpdateState::MergeCompleted);
}

auto SnapshotManager::CheckTargetMergeState(LockedFile* lock, const std::string& name,
                                            const SnapshotUpdateStatus& update_status)
        -> MergeResult {
    SnapshotStatus snapshot_status;
    if (!ReadSnapshotStatus(lock, name, &snapshot_status)) {
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::ReadStatus);
    }

    std::unique_ptr<LpMetadata> current_metadata;

    if (!IsSnapshotDevice(name)) {
        if (!current_metadata) {
            current_metadata = ReadCurrentMetadata();
        }

        if (!current_metadata ||
            GetMetadataPartitionState(*current_metadata, name) != MetadataPartitionState::Updated) {
            DeleteSnapshot(lock, name);
            return MergeResult(UpdateState::Cancelled);
        }

        // During a check, we decided the merge was complete, but we were unable to
        // collapse the device-mapper stack and perform COW cleanup. If we haven't
        // rebooted after this check, the device will still be a snapshot-merge
        // target. If we have rebooted, the device will now be a linear target,
        // and we can try cleanup again.
        if (snapshot_status.state() == SnapshotState::MERGE_COMPLETED) {
            // NB: It's okay if this fails now, we gave cleanup our best effort.
            OnSnapshotMergeComplete(lock, name, snapshot_status);
            return MergeResult(UpdateState::MergeCompleted);
        }

        LOG(ERROR) << "Expected snapshot or snapshot-merge for device: " << name;
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::UnknownTargetType);
    }

    // This check is expensive so it is only enabled for debugging.
    DCHECK((current_metadata = ReadCurrentMetadata()) &&
           GetMetadataPartitionState(*current_metadata, name) == MetadataPartitionState::Updated);

    if (UpdateUsesUserSnapshots(lock)) {
        if (!EnsureSnapuserdConnected()) {
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::QuerySnapshotStatus);
        }

        // Query the snapshot status from the daemon
        const auto merge_status = snapuserd_client_->QuerySnapshotStatus(name);
        if (merge_status == "snapshot-merge-failed") {
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::UnknownTargetType);
        }

        // This is the case when device reboots during merge. Once the device boots,
        // snapuserd daemon will not resume merge immediately in first stage init.
        // This is slightly different as compared to dm-snapshot-merge; In this
        // case, metadata file will have "MERGING" state whereas the daemon will be
        // waiting to resume the merge. Thus, we resume the merge at this point.
        if (merge_status == "snapshot" && snapshot_status.state() == SnapshotState::MERGING) {
            if (!snapuserd_client_->InitiateMerge(name)) {
                return MergeResult(UpdateState::MergeFailed, MergeFailureCode::UnknownTargetType);
            }
            return MergeResult(UpdateState::Merging);
        }

        if (merge_status == "snapshot" &&
            DecideMergePhase(snapshot_status) == MergePhase::SECOND_PHASE) {
            if (update_status.merge_phase() == MergePhase::FIRST_PHASE) {
                // The snapshot is not being merged because it's in the wrong phase.
                return MergeResult(UpdateState::None);
            } else {
                // update_status is already in second phase but the
                // snapshot_status is still not set to SnapshotState::MERGING.
                //
                // Resume the merge at this point. see b/374225913
                LOG(INFO) << "SwitchSnapshotToMerge: " << name << " after resuming merge";
                auto code = SwitchSnapshotToMerge(lock, name);
                if (code != MergeFailureCode::Ok) {
                    LOG(ERROR) << "Failed to switch snapshot: " << name
                               << " to merge during second phase";
                    return MergeResult(UpdateState::MergeFailed,
                                       MergeFailureCode::UnknownTargetType);
                }
                return MergeResult(UpdateState::Merging);
            }
        }

        if (merge_status == "snapshot-merge") {
            if (snapshot_status.state() == SnapshotState::MERGE_COMPLETED) {
                LOG(ERROR) << "Snapshot " << name
                           << " is merging after being marked merge-complete.";
                return MergeResult(UpdateState::MergeFailed,
                                   MergeFailureCode::UnmergedSectorsAfterCompletion);
            }
            return MergeResult(UpdateState::Merging);
        }

        if (merge_status != "snapshot-merge-complete") {
            LOG(ERROR) << "Snapshot " << name << " has incorrect status: " << merge_status;
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::ExpectedMergeTarget);
        }
    } else {
        // dm-snapshot in the kernel
        std::string target_type;
        DmTargetSnapshot::Status status;
        if (!QuerySnapshotStatus(name, &target_type, &status)) {
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::QuerySnapshotStatus);
        }
        if (target_type == "snapshot" &&
            DecideMergePhase(snapshot_status) == MergePhase::SECOND_PHASE &&
            update_status.merge_phase() == MergePhase::FIRST_PHASE) {
            // The snapshot is not being merged because it's in the wrong phase.
            return MergeResult(UpdateState::None);
        }
        if (target_type != "snapshot-merge") {
            // We can get here if we failed to rewrite the target type in
            // InitiateMerge(). If we failed to create the target in first-stage
            // init, boot would not succeed.
            LOG(ERROR) << "Snapshot " << name << " has incorrect target type: " << target_type;
            return MergeResult(UpdateState::MergeFailed, MergeFailureCode::ExpectedMergeTarget);
        }

        // These two values are equal when merging is complete.
        if (status.sectors_allocated != status.metadata_sectors) {
            if (snapshot_status.state() == SnapshotState::MERGE_COMPLETED) {
                LOG(ERROR) << "Snapshot " << name
                           << " is merging after being marked merge-complete.";
                return MergeResult(UpdateState::MergeFailed,
                                   MergeFailureCode::UnmergedSectorsAfterCompletion);
            }
            return MergeResult(UpdateState::Merging);
        }
    }

    // Merging is done. First, update the status file to indicate the merge
    // is complete. We do this before calling OnSnapshotMergeComplete, even
    // though this means the write is potentially wasted work (since in the
    // ideal case we'll immediately delete the file).
    //
    // This makes it simpler to reason about the next reboot: no matter what
    // part of cleanup failed, first-stage init won't try to create another
    // snapshot device for this partition.
    snapshot_status.set_state(SnapshotState::MERGE_COMPLETED);
    if (!WriteSnapshotStatus(lock, snapshot_status)) {
        return MergeResult(UpdateState::MergeFailed, MergeFailureCode::WriteStatus);
    }
    if (!OnSnapshotMergeComplete(lock, name, snapshot_status)) {
        return MergeResult(UpdateState::MergeNeedsReboot);
    }
    return MergeResult(UpdateState::MergeCompleted, MergeFailureCode::Ok);
}

// This returns the backing device, not the dm-user layer.
static std::string GetMappedCowDeviceName(const std::string& snapshot,
                                          const SnapshotStatus& status) {
    // If no partition was created (the COW exists entirely on /data), the
    // device-mapper layering is different than if we had a partition.
    if (status.cow_partition_size() == 0) {
        return GetCowImageDeviceName(snapshot);
    }
    return GetCowName(snapshot);
}

MergeFailureCode SnapshotManager::MergeSecondPhaseSnapshots(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        return MergeFailureCode::ListSnapshots;
    }

    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    CHECK(update_status.state() == UpdateState::Merging ||
          update_status.state() == UpdateState::MergeFailed);
    CHECK(update_status.merge_phase() == MergePhase::FIRST_PHASE);

    update_status.set_state(UpdateState::Merging);
    update_status.set_merge_phase(MergePhase::SECOND_PHASE);
    if (!WriteSnapshotUpdateStatus(lock, update_status)) {
        return MergeFailureCode::WriteStatus;
    }

    auto current_slot_suffix = device_->GetSlotSuffix();
    MergeFailureCode result = MergeFailureCode::Ok;
    for (const auto& snapshot : snapshots) {
        if (!android::base::EndsWith(snapshot, current_slot_suffix)) {
            LOG(ERROR) << "Skipping invalid snapshot: " << snapshot
                       << " during MergeSecondPhaseSnapshots";
            continue;
        }
        SnapshotStatus snapshot_status;
        if (!ReadSnapshotStatus(lock, snapshot, &snapshot_status)) {
            return MergeFailureCode::ReadStatus;
        }
        if (DecideMergePhase(snapshot_status) != MergePhase::SECOND_PHASE) {
            continue;
        }
        auto code = SwitchSnapshotToMerge(lock, snapshot);
        if (code != MergeFailureCode::Ok) {
            LOG(ERROR) << "Failed to switch snapshot to a second-phase merge target: " << snapshot;
            if (result == MergeFailureCode::Ok) {
                result = code;
            }
        }
    }
    return result;
}

std::string SnapshotManager::GetBootSnapshotsWithoutSlotSwitchPath() {
    return metadata_dir_ + "/" + android::base::Basename(kBootSnapshotsWithoutSlotSwitch);
}

std::string SnapshotManager::GetSnapshotBootIndicatorPath() {
    return metadata_dir_ + "/" + android::base::Basename(kBootIndicatorPath);
}

std::string SnapshotManager::GetRollbackIndicatorPath() {
    return metadata_dir_ + "/" + android::base::Basename(kRollbackIndicatorPath);
}

std::string SnapshotManager::GetSnapuserdFromSystemPath() {
    return metadata_dir_ + "/" + android::base::Basename(kSnapuserdFromSystem);
}

std::string SnapshotManager::GetForwardMergeIndicatorPath() {
    return metadata_dir_ + "/allow-forward-merge";
}

std::string SnapshotManager::GetOldPartitionMetadataPath() {
    return metadata_dir_ + "/old-partition-metadata";
}

void SnapshotManager::AcknowledgeMergeSuccess(LockedFile* lock) {
    // It's not possible to remove update state in recovery, so write an
    // indicator that cleanup is needed on reboot. If a factory data reset
    // was requested, it doesn't matter, everything will get wiped anyway.
    // To make testing easier we consider a /data wipe as cleaned up.
    if (device_->IsRecovery()) {
        WriteUpdateState(lock, UpdateState::MergeCompleted);
        return;
    }

    RemoveAllUpdateState(lock);

    if (UpdateUsesUserSnapshots(lock) && !device()->IsTestDevice()) {
        if (snapuserd_client_) {
            snapuserd_client_->DetachSnapuserd();
            snapuserd_client_->RemoveTransitionedDaemonIndicator();
            snapuserd_client_ = nullptr;
        }
    }
}

void SnapshotManager::AcknowledgeMergeFailure(MergeFailureCode failure_code) {
    // Log first, so worst case, we always have a record of why the calls below
    // were being made.
    LOG(ERROR) << "Merge could not be completed and will be marked as failed.";

    auto lock = LockExclusive();
    if (!lock) return;

    // Since we released the lock in between WaitForMerge and here, it's
    // possible (1) the merge successfully completed or (2) was already
    // marked as a failure. So make sure to check the state again, and
    // only mark as a failure if appropriate.
    UpdateState state = ReadUpdateState(lock.get());
    if (state != UpdateState::Merging && state != UpdateState::MergeNeedsReboot) {
        return;
    }

    WriteUpdateState(lock.get(), UpdateState::MergeFailed, failure_code);
}

bool SnapshotManager::OnSnapshotMergeComplete(LockedFile* lock, const std::string& name,
                                              const SnapshotStatus& status) {
    if (!UpdateUsesUserSnapshots(lock)) {
        if (IsSnapshotDevice(name)) {
            // We are extra-cautious here, to avoid deleting the wrong table.
            std::string target_type;
            DmTargetSnapshot::Status dm_status;
            if (!QuerySnapshotStatus(name, &target_type, &dm_status)) {
                return false;
            }
            if (target_type != "snapshot-merge") {
                LOG(ERROR) << "Unexpected target type " << target_type
                           << " for snapshot device: " << name;
                return false;
            }
            if (dm_status.sectors_allocated != dm_status.metadata_sectors) {
                LOG(ERROR) << "Merge is unexpectedly incomplete for device " << name;
                return false;
            }
            if (!CollapseSnapshotDevice(lock, name, status)) {
                LOG(ERROR) << "Unable to collapse snapshot: " << name;
                return false;
            }
        }
    } else {
        // Just collapse the device - no need to query again as we just did
        // prior to calling this function
        if (!CollapseSnapshotDevice(lock, name, status)) {
            LOG(ERROR) << "Unable to collapse snapshot: " << name;
            return false;
        }
    }

    // Note that collapsing is implicitly an Unmap, so we don't need to
    // unmap the snapshot.

    if (!DeleteSnapshot(lock, name)) {
        LOG(ERROR) << "Could not delete snapshot: " << name;
        return false;
    }
    return true;
}

bool SnapshotManager::CollapseSnapshotDevice(LockedFile* lock, const std::string& name,
                                             const SnapshotStatus& status) {
    if (!UpdateUsesUserSnapshots(lock)) {
        // Verify we have a snapshot-merge device.
        DeviceMapper::TargetInfo target;
        if (!GetSingleTarget(name, TableQuery::Table, &target)) {
            return false;
        }
        if (DeviceMapper::GetTargetType(target.spec) != "snapshot-merge") {
            // This should be impossible, it was checked earlier.
            LOG(ERROR) << "Snapshot device has invalid target type: " << name;
            return false;
        }

        std::string base_device, cow_device;
        if (!DmTargetSnapshot::GetDevicesFromParams(target.data, &base_device, &cow_device)) {
            LOG(ERROR) << "Could not parse snapshot device " << name
                       << " parameters: " << target.data;
            return false;
        }
    }

    uint64_t snapshot_sectors = status.snapshot_size() / kSectorSize;
    if (snapshot_sectors * kSectorSize != status.snapshot_size()) {
        LOG(ERROR) << "Snapshot " << name
                   << " size is not sector aligned: " << status.snapshot_size();
        return false;
    }

    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    // Create a DmTable that is identical to the base device.
    CreateLogicalPartitionParams base_device_params{
            .block_device = device_->GetSuperDevice(slot),
            .metadata_slot = slot,
            .partition_name = name,
            .partition_opener = &device_->GetPartitionOpener(),
    };
    DmTable table;
    if (!CreateDmTable(base_device_params, &table)) {
        LOG(ERROR) << "Could not create a DmTable for partition: " << name;
        return false;
    }

    if (!dm_.LoadTableAndActivate(name, table)) {
        return false;
    }

    if (!UpdateUsesUserSnapshots(lock)) {
        // Attempt to delete the snapshot device if one still exists. Nothing
        // should be depending on the device, and device-mapper should have
        // flushed remaining I/O. We could in theory replace with dm-zero (or
        // re-use the table above), but for now it's better to know why this
        // would fail.
        //
        // Furthermore, we should not be trying to unmap for userspace snapshot
        // as unmap will fail since dm-user itself was a snapshot device prior
        // to switching of tables. Unmap will fail as the device will be mounted
        // by system partitions
        if (status.using_snapuserd()) {
            auto dm_user_name = GetDmUserCowName(name, GetSnapshotDriver(lock));
            UnmapDmUserDevice(dm_user_name);
        }
    }

    // We can't delete base device immediately as daemon holds a reference.
    // Make sure we wait for all the worker threads to terminate and release
    // the reference
    if (UpdateUsesUserSnapshots(lock) && EnsureSnapuserdConnected()) {
        if (!snapuserd_client_->WaitForDeviceDelete(name)) {
            LOG(ERROR) << "Failed to wait for " << name << " control device to delete";
        }
    }

    auto base_name = GetBaseDeviceName(name);
    if (!DeleteDeviceIfExists(base_name)) {
        LOG(ERROR) << "Unable to delete base device for snapshot: " << base_name;
    }

    if (!DeleteDeviceIfExists(GetSourceDeviceName(name), 4000ms)) {
        LOG(ERROR) << "Unable to delete source device for snapshot: " << GetSourceDeviceName(name);
    }

    return true;
}

bool SnapshotManager::HandleCancelledUpdate(LockedFile* lock,
                                            const std::function<bool()>& before_cancel) {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Unknown) {
        return false;
    }

    // If all snapshots were reflashed, then cancel the entire update.
    if (AreAllSnapshotsCancelled(lock)) {
        LOG(WARNING) << "Detected re-flashing, cancelling unverified update.";
        return RemoveAllUpdateState(lock, before_cancel);
    }

    // If update has been rolled back, then cancel the entire update.
    // Client (update_engine) is responsible for doing additional cleanup work on its own states
    // when ProcessUpdateState() returns UpdateState::Cancelled.
    auto current_slot = GetCurrentSlot();
    if (current_slot != Slot::Source) {
        LOG(INFO) << "Update state is being processed while booting at " << current_slot
                  << " slot, taking no action.";
        return false;
    }

    // current_slot == Source. Attempt to detect rollbacks.
    if (access(GetRollbackIndicatorPath().c_str(), F_OK) != 0) {
        // This unverified update is not attempted. Take no action.
        PLOG(INFO) << "Rollback indicator not detected. "
                   << "Update state is being processed before reboot, taking no action.";
        return false;
    }

    LOG(WARNING) << "Detected rollback, cancelling unverified update.";
    return RemoveAllUpdateState(lock, before_cancel);
}

bool SnapshotManager::PerformInitTransition(InitTransition transition,
                                            std::vector<std::string>* snapuserd_argv) {
    LOG(INFO) << "Performing transition for snapuserd.";

    // Don't use EnsureSnapuserdConnected() because this is called from init,
    // and attempting to do so will deadlock.
    if (!snapuserd_client_ && transition != InitTransition::SELINUX_DETACH) {
        snapuserd_client_ = SnapuserdClient::Connect(kSnapuserdSocket, 10s);
        if (!snapuserd_client_) {
            LOG(ERROR) << "Unable to connect to snapuserd";
            return false;
        }
    }

    auto lock = LockExclusive();
    if (!lock) return false;

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        LOG(ERROR) << "Failed to list snapshots.";
        return false;
    }

    if (UpdateUsesUserSnapshots(lock.get()) && transition == InitTransition::SELINUX_DETACH) {
        snapuserd_argv->emplace_back("-user_snapshot");
        if (UpdateUsesIouring(lock.get())) {
            snapuserd_argv->emplace_back("-io_uring");
        }
        if (UpdateUsesODirect(lock.get())) {
            snapuserd_argv->emplace_back("-o_direct");
        }
        uint cow_op_merge_size = GetUpdateCowOpMergeSize(lock.get());
        if (cow_op_merge_size != 0) {
            snapuserd_argv->emplace_back("-cow_op_merge_size=" + std::to_string(cow_op_merge_size));
        }
        uint32_t worker_count = GetUpdateWorkerCount(lock.get());
        if (worker_count != 0) {
            snapuserd_argv->emplace_back("-worker_count=" + std::to_string(worker_count));
        }
        uint32_t verify_block_size = GetVerificationBlockSize(lock.get());
        if (verify_block_size != 0) {
            snapuserd_argv->emplace_back("-verify_block_size=" + std::to_string(verify_block_size));
        }
        uint32_t num_verify_threads = GetNumVerificationThreads(lock.get());
        if (num_verify_threads != 0) {
            snapuserd_argv->emplace_back("-num_verify_threads=" +
                                         std::to_string(num_verify_threads));
        }
    }

    size_t num_cows = 0;
    size_t ok_cows = 0;
    for (const auto& snapshot : snapshots) {
        std::string user_cow_name = GetDmUserCowName(snapshot, GetSnapshotDriver(lock.get()));

        if (dm_.GetState(user_cow_name) == DmDeviceState::INVALID) {
            continue;
        }

        DeviceMapper::TargetInfo target;
        if (!GetSingleTarget(user_cow_name, TableQuery::Table, &target)) {
            continue;
        }

        auto target_type = DeviceMapper::GetTargetType(target.spec);
        if (target_type != "user") {
            LOG(ERROR) << "Unexpected target type for " << user_cow_name << ": " << target_type;
            continue;
        }

        num_cows++;

        SnapshotStatus snapshot_status;
        if (!ReadSnapshotStatus(lock.get(), snapshot, &snapshot_status)) {
            LOG(ERROR) << "Unable to read snapshot status: " << snapshot;
            continue;
        }

        auto misc_name = user_cow_name;

        std::string source_device_name;
        if (snapshot_status.old_partition_size() > 0) {
            source_device_name = GetSourceDeviceName(snapshot);
        } else {
            source_device_name = GetBaseDeviceName(snapshot);
        }

        std::string source_device;
        if (!dm_.GetDmDevicePathByName(source_device_name, &source_device)) {
            LOG(ERROR) << "Could not get device path for " << GetSourceDeviceName(snapshot);
            continue;
        }

        std::string base_path_merge;
        if (!dm_.GetDmDevicePathByName(GetBaseDeviceName(snapshot), &base_path_merge)) {
            LOG(ERROR) << "Could not get device path for " << GetSourceDeviceName(snapshot);
            continue;
        }

        std::string cow_image_name = GetMappedCowDeviceName(snapshot, snapshot_status);

        std::string cow_image_device;
        if (!dm_.GetDmDevicePathByName(cow_image_name, &cow_image_device)) {
            LOG(ERROR) << "Could not get device path for " << cow_image_name;
            continue;
        }

        if (transition == InitTransition::SELINUX_DETACH) {
            if (!UpdateUsesUserSnapshots(lock.get())) {
                auto message = misc_name + "," + cow_image_device + "," + source_device;
                snapuserd_argv->emplace_back(std::move(message));
            } else {
                auto message = misc_name + "," + cow_image_device + "," + source_device + "," +
                               base_path_merge;
                snapuserd_argv->emplace_back(std::move(message));
            }
            SetReadAheadSize(cow_image_device, snapshot_status.read_ahead_size());
            SetReadAheadSize(source_device, snapshot_status.read_ahead_size());

            // Do not attempt to connect to the new snapuserd yet, it hasn't
            // been started. We do however want to wait for the misc device
            // to have been created.
            ok_cows++;
            continue;
        }

        DmTable table;
        table.Emplace<DmTargetUser>(0, target.spec.length, misc_name);
        if (!dm_.LoadTableAndActivate(user_cow_name, table)) {
            LOG(ERROR) << "Unable to swap tables for " << misc_name;
            continue;
        }

        // Wait for ueventd to acknowledge and create the control device node.
        std::string control_device = "/dev/dm-user/" + misc_name;
        if (!WaitForDevice(control_device, 10s)) {
            LOG(ERROR) << "dm-user control device no found:  " << misc_name;
            continue;
        }

        uint64_t base_sectors;
        if (!UpdateUsesUserSnapshots(lock.get())) {
            base_sectors =
                    snapuserd_client_->InitDmUserCow(misc_name, cow_image_device, source_device);
        } else {
            base_sectors = snapuserd_client_->InitDmUserCow(misc_name, cow_image_device,
                                                            source_device, base_path_merge);
        }

        if (base_sectors == 0) {
            // Unrecoverable as metadata reads from cow device failed
            LOG(FATAL) << "Failed to retrieve base_sectors from Snapuserd";
            return false;
        }

        CHECK(base_sectors <= target.spec.length);

        if (!snapuserd_client_->AttachDmUser(misc_name)) {
            // This error is unrecoverable. We cannot proceed because reads to
            // the underlying device will fail.
            LOG(FATAL) << "Could not initialize snapuserd for " << user_cow_name;
            return false;
        }

        ok_cows++;
    }

    if (ok_cows != num_cows) {
        LOG(ERROR) << "Could not transition all snapuserd consumers.";
        return false;
    }
    return true;
}

std::unique_ptr<LpMetadata> SnapshotManager::ReadCurrentMetadata() {
    const auto& opener = device_->GetPartitionOpener();
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto super_device = device_->GetSuperDevice(slot);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
        return nullptr;
    }
    return metadata;
}

SnapshotManager::MetadataPartitionState SnapshotManager::GetMetadataPartitionState(
        const LpMetadata& metadata, const std::string& name) {
    auto partition = android::fs_mgr::FindPartition(metadata, name);
    if (!partition) return MetadataPartitionState::None;
    if (partition->attributes & LP_PARTITION_ATTR_UPDATED) {
        return MetadataPartitionState::Updated;
    }
    return MetadataPartitionState::Flashed;
}

bool SnapshotManager::AreAllSnapshotsCancelled(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        LOG(WARNING) << "Failed to list snapshots to determine whether device has been flashed "
                     << "after applying an update. Assuming no snapshots.";
        // Let HandleCancelledUpdate resets UpdateState.
        return true;
    }

    std::map<std::string, bool> flashing_status;

    if (!GetSnapshotFlashingStatus(lock, snapshots, &flashing_status)) {
        LOG(WARNING) << "Failed to determine whether partitions have been flashed. Not"
                     << "removing update states.";
        return false;
    }

    bool all_snapshots_cancelled = std::all_of(flashing_status.begin(), flashing_status.end(),
                                               [](const auto& pair) { return pair.second; });

    if (all_snapshots_cancelled) {
        LOG(WARNING) << "All partitions are re-flashed after update, removing all update states.";
    }
    return all_snapshots_cancelled;
}

bool SnapshotManager::GetSnapshotFlashingStatus(LockedFile* lock,
                                                const std::vector<std::string>& snapshots,
                                                std::map<std::string, bool>* out) {
    CHECK(lock);

    auto source_slot_suffix = ReadUpdateSourceSlotSuffix();
    if (source_slot_suffix.empty()) {
        return false;
    }
    uint32_t source_slot = SlotNumberForSlotSuffix(source_slot_suffix);
    uint32_t target_slot = (source_slot == 0) ? 1 : 0;

    // Attempt to detect re-flashing on each partition.
    // - If all partitions are re-flashed, we can proceed to cancel the whole update.
    // - If only some of the partitions are re-flashed, snapshots for re-flashed partitions are
    //   deleted. Caller is responsible for merging the rest of the snapshots.
    // - If none of the partitions are re-flashed, caller is responsible for merging the snapshots.
    //
    // Note that we use target slot metadata, since if an OTA has been applied
    // to the target slot, we can detect the UPDATED flag. Any kind of flash
    // operation against dynamic partitions ensures that all copies of the
    // metadata are in sync, so flashing all partitions on the source slot will
    // remove the UPDATED flag on the target slot as well.
    const auto& opener = device_->GetPartitionOpener();
    auto super_device = device_->GetSuperDevice(target_slot);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, target_slot);
    if (!metadata) {
        return false;
    }

    for (const auto& snapshot_name : snapshots) {
        if (GetMetadataPartitionState(*metadata, snapshot_name) ==
            MetadataPartitionState::Updated) {
            out->emplace(snapshot_name, false);
        } else {
            // Delete snapshots for partitions that are re-flashed after the update.
            LOG(WARNING) << "Detected re-flashing of partition " << snapshot_name << ".";
            out->emplace(snapshot_name, true);
        }
    }
    return true;
}

void SnapshotManager::RemoveInvalidSnapshots(LockedFile* lock) {
    std::vector<std::string> snapshots;

    // Remove the stale snapshot metadata
    //
    // We make sure that all the three cases
    // are valid before removing the snapshot metadata:
    //
    // 1: dm state is active
    // 2: Root fs is not mounted off as a snapshot device
    // 3: Snapshot slot suffix should match current device slot
    if (!ListSnapshots(lock, &snapshots, device_->GetSlotSuffix()) || snapshots.empty()) {
        return;
    }

    // We indeed have some invalid snapshots
    for (const auto& name : snapshots) {
        if (dm_.GetState(name) == DmDeviceState::ACTIVE && !IsSnapshotDevice(name)) {
            if (!DeleteSnapshot(lock, name)) {
                LOG(ERROR) << "Failed to delete invalid snapshot: " << name;
            } else {
                LOG(INFO) << "Invalid snapshot: " << name << " deleted";
            }
        }
    }
}

bool SnapshotManager::RemoveAllSnapshots(LockedFile* lock) {
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        return false;
    }

    std::map<std::string, bool> flashing_status;
    if (!GetSnapshotFlashingStatus(lock, snapshots, &flashing_status)) {
        LOG(WARNING) << "Failed to get flashing status";
    }

    auto current_slot = GetCurrentSlot();
    bool ok = true;
    bool has_mapped_cow_images = false;
    for (const auto& name : snapshots) {
        // If booting off source slot, it is okay to unmap and delete all the snapshots.
        // If boot indicator is missing, update state is None or Initiated, so
        //   it is also okay to unmap and delete all the snapshots.
        // If booting off target slot,
        //  - should not unmap because:
        //    - In Android mode, snapshots are not mapped, but
        //      filesystems are mounting off dm-linear targets directly.
        //    - In recovery mode, assume nothing is mapped, so it is optional to unmap.
        //  - If partition is flashed or unknown, it is okay to delete snapshots.
        //    Otherwise (UPDATED flag), only delete snapshots if they are not mapped
        //    as dm-snapshot (for example, after merge completes).
        bool should_unmap = current_slot != Slot::Target;
        bool should_delete = ShouldDeleteSnapshot(flashing_status, current_slot, name);
        if (should_unmap && android::base::EndsWith(name, device_->GetSlotSuffix())) {
            // Something very unexpected has happened - we want to unmap this
            // snapshot, but it's on the wrong slot. We can't unmap an active
            // partition. If this is not really a snapshot, skip the unmap
            // step.
            if (dm_.GetState(name) == DmDeviceState::INVALID || !IsSnapshotDevice(name)) {
                LOG(ERROR) << "Detected snapshot " << name << " on " << current_slot << " slot"
                           << " for source partition; removing without unmap.";
                should_unmap = false;
            }
        }

        bool partition_ok = true;
        if (should_unmap && !UnmapPartitionWithSnapshot(lock, name)) {
            partition_ok = false;
        }
        if (partition_ok && should_delete && !DeleteSnapshot(lock, name)) {
            partition_ok = false;
        }

        if (!partition_ok) {
            // Remember whether or not we were able to unmap the cow image.
            auto cow_image_device = GetCowImageDeviceName(name);
            has_mapped_cow_images |=
                    (EnsureImageManager() && images_->IsImageMapped(cow_image_device));

            ok = false;
        }
    }

    if (ok || !has_mapped_cow_images) {
        if (!EnsureImageManager()) {
            return false;
        }

        if (device_->IsRecovery()) {
            // If a device is in recovery, we need to mark the snapshots for cleanup
            // upon next reboot, since we cannot delete them here.
            if (!images_->DisableAllImages()) {
                LOG(ERROR) << "Could not remove all snapshot artifacts in recovery";
                return false;
            }
        } else if (!images_->RemoveAllImages()) {
            // Delete any image artifacts as a precaution, in case an update is
            // being cancelled due to some corrupted state in an lp_metadata file.
            // Note that we do not do this if some cow images are still mapped,
            // since we must not remove backing storage if it's in use.
            LOG(ERROR) << "Could not remove all snapshot artifacts";
            return false;
        }
    }
    return ok;
}

// See comments in RemoveAllSnapshots().
bool SnapshotManager::ShouldDeleteSnapshot(const std::map<std::string, bool>& flashing_status,
                                           Slot current_slot, const std::string& name) {
    if (current_slot != Slot::Target) {
        return true;
    }
    auto it = flashing_status.find(name);
    if (it == flashing_status.end()) {
        LOG(WARNING) << "Can't determine flashing status for " << name;
        return true;
    }
    if (it->second) {
        // partition flashed, okay to delete obsolete snapshots
        return true;
    }
    return !IsSnapshotDevice(name);
}

UpdateState SnapshotManager::GetUpdateState(double* progress) {
    // If we've never started an update, the state file won't exist.
    auto state_file = GetStateFilePath();
    if (access(state_file.c_str(), F_OK) != 0 && errno == ENOENT) {
        return UpdateState::None;
    }

    auto lock = LockShared();
    if (!lock) {
        return UpdateState::None;
    }

    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock.get());
    auto state = update_status.state();
    if (progress == nullptr) {
        return state;
    }

    if (state == UpdateState::MergeCompleted) {
        *progress = 100.0;
        return state;
    }

    *progress = 0.0;
    if (state != UpdateState::Merging) {
        return state;
    }

    if (!UpdateUsesUserSnapshots(lock.get())) {
        // Sum all the snapshot states as if the system consists of a single huge
        // snapshots device, then compute the merge completion percentage of that
        // device.
        std::vector<std::string> snapshots;
        if (!ListSnapshots(lock.get(), &snapshots)) {
            LOG(ERROR) << "Could not list snapshots";
            return state;
        }

        DmTargetSnapshot::Status fake_snapshots_status = {};
        for (const auto& snapshot : snapshots) {
            DmTargetSnapshot::Status current_status;

            if (!IsSnapshotDevice(snapshot)) continue;
            if (!QuerySnapshotStatus(snapshot, nullptr, &current_status)) continue;

            fake_snapshots_status.sectors_allocated += current_status.sectors_allocated;
            fake_snapshots_status.total_sectors += current_status.total_sectors;
            fake_snapshots_status.metadata_sectors += current_status.metadata_sectors;
        }

        *progress = DmTargetSnapshot::MergePercent(fake_snapshots_status,
                                                   update_status.sectors_allocated());
    } else {
        if (EnsureSnapuserdConnected()) {
            *progress = snapuserd_client_->GetMergePercent();
        }
    }

    return state;
}

bool SnapshotManager::IsSnapshotWithoutSlotSwitch() {
    return (access(GetBootSnapshotsWithoutSlotSwitchPath().c_str(), F_OK) == 0);
}

bool SnapshotManager::UpdateUsesCompression() {
    auto lock = LockShared();
    if (!lock) return false;
    return UpdateUsesCompression(lock.get());
}

bool SnapshotManager::UpdateUsesCompression(LockedFile* lock) {
    // This returns true even if compression is "none", since update_engine is
    // really just trying to see if snapuserd is in use.
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.using_snapuserd();
}

bool SnapshotManager::UpdateUsesIouring(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.io_uring_enabled();
}

bool SnapshotManager::UpdateUsesODirect(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.o_direct();
}

bool SnapshotManager::UpdateUsesSkipVerification(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.skip_verification();
}

uint32_t SnapshotManager::GetUpdateCowOpMergeSize(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.cow_op_merge_size();
}

uint32_t SnapshotManager::GetUpdateWorkerCount(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.num_worker_threads();
}

uint32_t SnapshotManager::GetVerificationBlockSize(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.verify_block_size();
}

uint32_t SnapshotManager::GetNumVerificationThreads(LockedFile* lock) {
    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
    return update_status.num_verification_threads();
}

bool SnapshotManager::MarkSnapuserdFromSystem() {
    auto path = GetSnapuserdFromSystemPath();

    if (!android::base::WriteStringToFile("1", path)) {
        PLOG(ERROR) << "Unable to write to vendor update path: " << path;
        return false;
    }

    unique_fd fd(open(path.c_str(), O_PATH));
    if (fd < 0) {
        PLOG(ERROR) << "Failed to open file: " << path;
        return false;
    }

    /*
     * This function is invoked by first stage init and hence we need to
     * explicitly set the correct selinux label for this file as update_engine
     * will try to remove this file later on once the snapshot merge is
     * complete.
     */
    if (fsetxattr(fd.get(), XATTR_NAME_SELINUX, kOtaFileContext, strlen(kOtaFileContext) + 1, 0) <
        0) {
        PLOG(ERROR) << "fsetxattr for the path: " << path << " failed";
    }

    return true;
}

/*
 * Please see b/304829384 for more details.
 *
 * In Android S, we use dm-snapshot for mounting snapshots and snapshot-merge
 * process. If the vendor partition continues to be on Android S, then
 * "snapuserd" binary in first stage ramdisk will be from vendor partition.
 * Thus, we need to maintain backward compatibility.
 *
 * Now, We take a two step approach to maintain the backward compatibility:
 *
 * 1: During OTA installation, we will continue to use "user-space" snapshots
 * for OTA installation as both update-engine and snapuserd binary will be from system partition.
 * However, during installation, we mark "legacy_snapuserd" in
 * SnapshotUpdateStatus file to mark that this is a path to support backward compatibility.
 * Thus, this function will return "false" during OTA installation.
 *
 * 2: Post OTA reboot, there are two key steps:
 *    a: During first stage init, "init" and "snapuserd" could be from vendor
 *    partition. This could be from Android S. Thus, the snapshot mount path
 *    will be based off dm-snapshot.
 *
 *    b: Post selinux transition, "init" and "update-engine" will be "system"
 *    partition. Now, since the snapshots are mounted off dm-snapshot,
 *    update-engine interaction with "snapuserd" should work based off
 *    dm-snapshots.
 *
 *    TL;DR: update-engine will use the "system" snapuserd for installing new
 *    updates (this is safe as there is no "vendor" snapuserd running during
 *    installation). Post reboot, update-engine will use the legacy path when
 *    communicating with "vendor" snapuserd that was started in first-stage
 *    init. Hence, this function checks:
 *         i: Are we in post OTA reboot
 *         ii: Is the Vendor from Android 12
 *         iii: If both (i) and (ii) are true, then use the dm-snapshot based
 *         approach.
 *
 * 3: Post OTA reboot, if the vendor partition was updated from Android 12 to
 * any other release post Android 12, then snapuserd binary will be "system"
 * partition as post Android 12, init_boot will contain a copy of snapuserd
 * binary. Thus, during first stage init, if init is able to communicate to
 * daemon, that gives us a signal that the binary is from "system" copy. Hence,
 * there is no need to fallback to legacy dm-snapshot. Thus, init will use a
 * marker in /metadata to signal that the snapuserd binary from first stage init
 * can handle userspace snapshots.
 *
 */
bool SnapshotManager::IsLegacySnapuserdPostReboot() {
    auto slot = GetCurrentSlot();
    if (slot == Slot::Target) {
        /*
            If this marker is present, the daemon can handle userspace snapshots.
            During post-OTA reboot, this implies that the vendor partition is
            Android 13 or higher. If the snapshots were created on an
            Android 12 vendor, this means the vendor partition has been updated.
        */
        if (access(GetSnapuserdFromSystemPath().c_str(), F_OK) == 0) {
            is_snapshot_userspace_ = true;
            return false;
        }
        // If the marker isn't present and if the vendor is still in Android 12
        if (is_legacy_snapuserd_.has_value() && is_legacy_snapuserd_.value() == true) {
            return true;
        }
    }

    return false;
}

bool SnapshotManager::UpdateUsesUserSnapshots() {
    // This and the following function is constantly
    // invoked during snapshot merge. We want to avoid
    // constantly reading from disk. Hence, store this
    // value in memory.
    //
    // Furthermore, this value in the disk is set
    // only when OTA is applied and doesn't change
    // during merge phase. Hence, once we know that
    // the value is read from disk the very first time,
    // it is safe to read successive checks from memory.

    if (is_snapshot_userspace_.has_value()) {
        // Check if legacy snapuserd is running post OTA reboot
        if (IsLegacySnapuserdPostReboot()) {
            return false;
        }
        return is_snapshot_userspace_.value();
    }

    auto lock = LockShared();
    if (!lock) return false;

    return UpdateUsesUserSnapshots(lock.get());
}

bool SnapshotManager::UpdateUsesUserSnapshots(LockedFile* lock) {
    if (!is_snapshot_userspace_.has_value()) {
        SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock);
        is_snapshot_userspace_ = update_status.userspace_snapshots();
        is_legacy_snapuserd_ = update_status.legacy_snapuserd();
    }

    if (IsLegacySnapuserdPostReboot()) {
        return false;
    }

    return is_snapshot_userspace_.value();
}

bool SnapshotManager::ListSnapshots(LockedFile* lock, std::vector<std::string>* snapshots,
                                    const std::string& suffix) {
    CHECK(lock);

    auto dir_path = metadata_dir_ + "/snapshots"s;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(dir_path.c_str()), closedir);
    if (!dir) {
        PLOG(ERROR) << "opendir failed: " << dir_path;
        return false;
    }

    struct dirent* dp;
    while ((dp = readdir(dir.get())) != nullptr) {
        if (dp->d_type != DT_REG) continue;

        std::string name(dp->d_name);
        if (!suffix.empty() && !android::base::EndsWith(name, suffix)) {
            continue;
        }

        // Insert system and product partition at the beginning so that
        // during snapshot-merge, these partitions are merged first.
        if (name == "system_a" || name == "system_b" || name == "product_a" ||
            name == "product_b") {
            snapshots->insert(snapshots->begin(), std::move(name));
        } else {
            snapshots->emplace_back(std::move(name));
        }
    }

    return true;
}

bool SnapshotManager::IsSnapshotManagerNeeded() {
    if (access(kBootIndicatorPath, F_OK) == 0) {
        return true;
    }

    if (IsScratchOtaMetadataOnSuper()) {
        return true;
    }

    return false;
}

bool SnapshotManager::MapTempOtaMetadataPartitionIfNeeded(
        const std::function<bool(const std::string&)>& init) {
    auto device = android::snapshot::GetScratchOtaMetadataPartition();
    if (!device.empty()) {
        init(device);
        if (android::snapshot::MapScratchOtaMetadataPartition(device).empty()) {
            return false;
        }
    }
    return true;
}

std::string SnapshotManager::GetGlobalRollbackIndicatorPath() {
    return kRollbackIndicatorPath;
}

bool SnapshotManager::NeedSnapshotsInFirstStageMount() {
    if (IsSnapshotWithoutSlotSwitch()) {
        if (GetCurrentSlot() != Slot::Source) {
            LOG(ERROR) << "Snapshots marked to boot without slot switch; but slot is wrong";
            return false;
        }
        return true;
    }
    // If we fail to read, we'll wind up using CreateLogicalPartitions, which
    // will create devices that look like the old slot, except with extra
    // content at the end of each device. This will confuse dm-verity, and
    // ultimately we'll fail to boot. Why not make it a fatal error and have
    // the reason be clearer? Because the indicator file still exists, and
    // if this was FATAL, reverting to the old slot would be broken.
    auto slot = GetCurrentSlot();

    if (slot != Slot::Target) {
        if (slot == Slot::Source) {
            // Device is rebooting into the original slot, so mark this as a
            // rollback.
            auto path = GetRollbackIndicatorPath();
            if (!android::base::WriteStringToFile("1", path)) {
                PLOG(ERROR) << "Unable to write rollback indicator: " << path;
            } else {
                LOG(INFO) << "Rollback detected, writing rollback indicator to " << path;
                if (device_->IsTempMetadata()) {
                    CleanupScratchOtaMetadataIfPresent();
                }
            }
        }
        LOG(INFO) << "Not booting from new slot. Will not mount snapshots.";
        return false;
    }

    // If we can't read the update state, it's unlikely anything else will
    // succeed, so this is a fatal error. We'll eventually exhaust boot
    // attempts and revert to the old slot.
    auto lock = LockShared();
    if (!lock) {
        LOG(FATAL) << "Could not read update state to determine snapshot status";
        return false;
    }
    switch (ReadUpdateState(lock.get())) {
        case UpdateState::Unverified:
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            return true;
        default:
            return false;
    }
}

bool SnapshotManager::CreateLogicalAndSnapshotPartitions(
        const std::string& super_device, const std::chrono::milliseconds& timeout_ms) {
    LOG(INFO) << "Creating logical partitions with snapshots as needed";

    auto lock = LockExclusive();
    if (!lock) return false;

    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    return MapAllPartitions(lock.get(), super_device, slot, timeout_ms);
}

bool SnapshotManager::MapAllPartitions(LockedFile* lock, const std::string& super_device,
                                       uint32_t slot, const std::chrono::milliseconds& timeout_ms) {
    const auto& opener = device_->GetPartitionOpener();
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
        return false;
    }

    if (!EnsureImageManager()) {
        return false;
    }

    for (const auto& partition : metadata->partitions) {
        if (GetPartitionGroupName(metadata->groups[partition.group_index]) == kCowGroupName) {
            LOG(INFO) << "Skip mapping partition " << GetPartitionName(partition) << " in group "
                      << kCowGroupName;
            continue;
        }

        if (GetPartitionName(partition) ==
            android::base::Basename(android::snapshot::kOtaMetadataMount)) {
            LOG(INFO) << "Partition: " << GetPartitionName(partition) << " skipping";
            continue;
        }

        CreateLogicalPartitionParams params = {
                .block_device = super_device,
                .metadata = metadata.get(),
                .partition = &partition,
                .timeout_ms = timeout_ms,
                .partition_opener = &opener,
        };
        if (!MapPartitionWithSnapshot(lock, std::move(params), SnapshotContext::Mount, nullptr)) {
            return false;
        }
    }

    LOG(INFO) << "Created logical partitions with snapshot.";
    return true;
}

static std::chrono::milliseconds GetRemainingTime(
        const std::chrono::milliseconds& timeout,
        const std::chrono::time_point<std::chrono::steady_clock>& begin) {
    // If no timeout is specified, execute all commands without specifying any timeout.
    if (timeout.count() == 0) return std::chrono::milliseconds(0);
    auto passed_time = std::chrono::steady_clock::now() - begin;
    auto remaining_time = timeout - duration_cast<std::chrono::milliseconds>(passed_time);
    if (remaining_time.count() <= 0) {
        LOG(ERROR) << "MapPartitionWithSnapshot has reached timeout " << timeout.count() << "ms ("
                   << remaining_time.count() << "ms remaining)";
        // Return min() instead of remaining_time here because 0 is treated as a special value for
        // no timeout, where the rest of the commands will still be executed.
        return std::chrono::milliseconds::min();
    }
    return remaining_time;
}

bool SnapshotManager::MapPartitionWithSnapshot(LockedFile* lock,
                                               CreateLogicalPartitionParams params,
                                               SnapshotContext context, SnapshotPaths* paths) {
    auto begin = std::chrono::steady_clock::now();

    CHECK(lock);

    if (params.GetPartitionName() != params.GetDeviceName()) {
        LOG(ERROR) << "Mapping snapshot with a different name is unsupported: partition_name = "
                   << params.GetPartitionName() << ", device_name = " << params.GetDeviceName();
        return false;
    }

    // Fill out fields in CreateLogicalPartitionParams so that we have more information (e.g. by
    // reading super partition metadata).
    CreateLogicalPartitionParams::OwnedData params_owned_data;
    if (!params.InitDefaults(&params_owned_data)) {
        return false;
    }

    if (!params.partition->num_extents) {
        LOG(INFO) << "Skipping zero-length logical partition: " << params.GetPartitionName();
        return true;  // leave path empty to indicate that nothing is mapped.
    }

    // Determine if there is a live snapshot for the SnapshotStatus of the partition; i.e. if the
    // partition still has a snapshot that needs to be mapped.  If no live snapshot or merge
    // completed, live_snapshot_status is set to nullopt.
    std::optional<SnapshotStatus> live_snapshot_status;
    do {
        if (!IsSnapshotWithoutSlotSwitch() &&
            !(params.partition->attributes & LP_PARTITION_ATTR_UPDATED)) {
            LOG(INFO) << "Detected re-flashing of partition, will skip snapshot: "
                      << params.GetPartitionName();
            break;
        }
        auto file_path = GetSnapshotStatusFilePath(params.GetPartitionName());
        if (access(file_path.c_str(), F_OK) != 0) {
            if (errno != ENOENT) {
                PLOG(INFO) << "Can't map snapshot for " << params.GetPartitionName()
                           << ": Can't access " << file_path;
                return false;
            }
            break;
        }
        live_snapshot_status = std::make_optional<SnapshotStatus>();
        if (!ReadSnapshotStatus(lock, params.GetPartitionName(), &*live_snapshot_status)) {
            return false;
        }
        // No live snapshot if merge is completed.
        if (live_snapshot_status->state() == SnapshotState::MERGE_COMPLETED) {
            live_snapshot_status.reset();
        }

        if (live_snapshot_status->state() == SnapshotState::NONE ||
            live_snapshot_status->cow_partition_size() + live_snapshot_status->cow_file_size() ==
                    0) {
            LOG(WARNING) << "Snapshot status for " << params.GetPartitionName()
                         << " is invalid, ignoring: state = "
                         << SnapshotState_Name(live_snapshot_status->state())
                         << ", cow_partition_size = " << live_snapshot_status->cow_partition_size()
                         << ", cow_file_size = " << live_snapshot_status->cow_file_size();
            live_snapshot_status.reset();
        }
    } while (0);

    if (live_snapshot_status.has_value()) {
        // dm-snapshot requires the base device to be writable.
        params.force_writable = true;
        // Map the base device with a different name to avoid collision.
        params.device_name = GetBaseDeviceName(params.GetPartitionName());
    }

    AutoDeviceList created_devices;

    // Create the base device for the snapshot, or if there is no snapshot, the
    // device itself. This device consists of the real blocks in the super
    // partition that this logical partition occupies.
    std::string base_path;
    if (!CreateLogicalPartition(params, &base_path)) {
        LOG(ERROR) << "Could not create logical partition " << params.GetPartitionName()
                   << " as device " << params.GetDeviceName();
        return false;
    }
    created_devices.EmplaceBack<AutoUnmapDevice>(&dm_, params.GetDeviceName());

    if (paths) {
        paths->target_device = base_path;
    }

    auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) {
        return false;
    }

    // Wait for the base device to appear
    if (!WaitForDevice(base_path, remaining_time)) {
        return false;
    }

    if (!live_snapshot_status.has_value()) {
        created_devices.Release();
        return true;
    }

    // We don't have ueventd in first-stage init, so use device major:minor
    // strings instead.
    std::string base_device;
    if (!dm_.GetDeviceString(params.GetDeviceName(), &base_device)) {
        LOG(ERROR) << "Could not determine major/minor for: " << params.GetDeviceName();
        return false;
    }

    remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;

    std::string cow_name;
    CreateLogicalPartitionParams cow_params = params;
    cow_params.timeout_ms = remaining_time;
    if (!MapCowDevices(lock, cow_params, *live_snapshot_status, &created_devices, &cow_name)) {
        return false;
    }
    std::string cow_device;
    if (!GetMappedImageDeviceStringOrPath(cow_name, &cow_device)) {
        LOG(ERROR) << "Could not determine major/minor for: " << cow_name;
        return false;
    }
    if (paths) {
        paths->cow_device_name = cow_name;
    }

    remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;

    if (context == SnapshotContext::Update && live_snapshot_status->using_snapuserd()) {
        // Stop here, we can't run dm-user yet, the COW isn't built.
        created_devices.Release();
        return true;
    }

    if (live_snapshot_status->using_snapuserd()) {
        // Get the source device (eg the view of the partition from before it was resized).
        std::string source_device_path;
        if (live_snapshot_status->old_partition_size() > 0) {
            if (!MapSourceDevice(lock, params.GetPartitionName(), remaining_time,
                                 &source_device_path)) {
                LOG(ERROR) << "Could not map source device for: " << cow_name;
                return false;
            }

            auto source_device = GetSourceDeviceName(params.GetPartitionName());
            created_devices.EmplaceBack<AutoUnmapDevice>(&dm_, source_device);
        } else {
            source_device_path = base_path;
        }

        if (!WaitForDevice(source_device_path, remaining_time)) {
            return false;
        }

        std::string cow_path;
        if (!GetMappedImageDevicePath(cow_name, &cow_path)) {
            LOG(ERROR) << "Could not determine path for: " << cow_name;
            return false;
        }
        if (!WaitForDevice(cow_path, remaining_time)) {
            return false;
        }

        auto name = GetDmUserCowName(params.GetPartitionName(), GetSnapshotDriver(lock));

        std::string new_cow_device;
        if (!MapDmUserCow(lock, name, cow_path, source_device_path, base_path, remaining_time,
                          &new_cow_device)) {
            LOG(ERROR) << "Could not map dm-user device for partition "
                       << params.GetPartitionName();
            return false;
        }
        created_devices.EmplaceBack<AutoUnmapDevice>(&dm_, name);

        cow_device = new_cow_device;
    }

    // For userspace snapshots, dm-user block device itself will act as a
    // snapshot device. There is one subtle difference - MapSnapshot will create
    // either snapshot target or snapshot-merge target based on the underlying
    // state of the snapshot device. If snapshot-merge target is created, merge
    // will immediately start in the kernel.
    //
    // This is no longer true with respect to userspace snapshots. When dm-user
    // block device is created, we just have the snapshots ready but daemon in
    // the user-space will not start the merge. We have to explicitly inform the
    // daemon to resume the merge. Check ProcessUpdateState() call stack.
    if (!UpdateUsesUserSnapshots(lock)) {
        remaining_time = GetRemainingTime(params.timeout_ms, begin);
        if (remaining_time.count() < 0) return false;

        std::string path;
        if (!MapSnapshot(lock, params.GetPartitionName(), base_device, cow_device, remaining_time,
                         &path)) {
            LOG(ERROR) << "Could not map snapshot for partition: " << params.GetPartitionName();
            return false;
        }
        // No need to add params.GetPartitionName() to created_devices since it is immediately
        // released.

        if (paths) {
            paths->snapshot_device = path;
        }
        LOG(INFO) << "Mapped " << params.GetPartitionName() << " as snapshot device at " << path;
    } else {
        LOG(INFO) << "Mapped " << params.GetPartitionName() << " as snapshot device at "
                  << cow_device;
    }

    created_devices.Release();

    return true;
}

bool SnapshotManager::UnmapPartitionWithSnapshot(LockedFile* lock,
                                                 const std::string& target_partition_name) {
    CHECK(lock);

    if (!UnmapSnapshot(lock, target_partition_name)) {
        return false;
    }

    if (!UnmapCowDevices(lock, target_partition_name)) {
        return false;
    }

    auto base_name = GetBaseDeviceName(target_partition_name);
    if (!DeleteDeviceIfExists(base_name)) {
        LOG(ERROR) << "Cannot delete base device: " << base_name;
        return false;
    }

    auto source_name = GetSourceDeviceName(target_partition_name);
    if (!DeleteDeviceIfExists(source_name)) {
        LOG(ERROR) << "Cannot delete source device: " << source_name;
        return false;
    }

    LOG(INFO) << "Successfully unmapped snapshot " << target_partition_name;

    return true;
}

bool SnapshotManager::MapCowDevices(LockedFile* lock, const CreateLogicalPartitionParams& params,
                                    const SnapshotStatus& snapshot_status,
                                    AutoDeviceList* created_devices, std::string* cow_name) {
    CHECK(lock);
    CHECK(snapshot_status.cow_partition_size() + snapshot_status.cow_file_size() > 0);
    auto begin = std::chrono::steady_clock::now();

    std::string partition_name = params.GetPartitionName();
    std::string cow_image_name = GetCowImageDeviceName(partition_name);
    *cow_name = GetCowName(partition_name);

    // Map COW image if necessary.
    if (snapshot_status.cow_file_size() > 0) {
        if (!EnsureImageManager()) return false;
        auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
        if (remaining_time.count() < 0) return false;

        if (!MapCowImage(partition_name, remaining_time).has_value()) {
            LOG(ERROR) << "Could not map cow image for partition: " << partition_name;
            return false;
        }
        created_devices->EmplaceBack<AutoUnmapImage>(images_.get(), cow_image_name);

        // If no COW partition exists, just return the image alone.
        if (snapshot_status.cow_partition_size() == 0) {
            *cow_name = std::move(cow_image_name);
            LOG(INFO) << "Mapped COW image for " << partition_name << " at " << *cow_name;
            return true;
        }
    }

    auto remaining_time = GetRemainingTime(params.timeout_ms, begin);
    if (remaining_time.count() < 0) return false;

    CHECK(snapshot_status.cow_partition_size() > 0);

    // Create the DmTable for the COW device. It is the DmTable of the COW partition plus
    // COW image device as the last extent.
    CreateLogicalPartitionParams cow_partition_params = params;
    cow_partition_params.partition = nullptr;
    cow_partition_params.partition_name = *cow_name;
    cow_partition_params.device_name.clear();
    DmTable table;
    if (!CreateDmTable(cow_partition_params, &table)) {
        return false;
    }
    // If the COW image exists, append it as the last extent.
    if (snapshot_status.cow_file_size() > 0) {
        std::string cow_image_device;
        if (!GetMappedImageDeviceStringOrPath(cow_image_name, &cow_image_device)) {
            LOG(ERROR) << "Cannot determine major/minor for: " << cow_image_name;
            return false;
        }
        auto cow_partition_sectors = snapshot_status.cow_partition_size() / kSectorSize;
        auto cow_image_sectors = snapshot_status.cow_file_size() / kSectorSize;
        table.Emplace<DmTargetLinear>(cow_partition_sectors, cow_image_sectors, cow_image_device,
                                      0);
    }

    // We have created the DmTable now. Map it.
    std::string cow_path;
    if (!dm_.CreateDevice(*cow_name, table, &cow_path, remaining_time)) {
        LOG(ERROR) << "Could not create COW device: " << *cow_name;
        return false;
    }
    created_devices->EmplaceBack<AutoUnmapDevice>(&dm_, *cow_name);
    LOG(INFO) << "Mapped COW device for " << params.GetPartitionName() << " at " << cow_path;
    return true;
}

bool SnapshotManager::UnmapCowDevices(LockedFile* lock, const std::string& name) {
    CHECK(lock);
    if (!EnsureImageManager()) return false;

    if (UpdateUsesCompression(lock) && !UpdateUsesUserSnapshots(lock)) {
        auto dm_user_name = GetDmUserCowName(name, GetSnapshotDriver(lock));
        if (!UnmapDmUserDevice(dm_user_name)) {
            return false;
        }
    }

    if (!DeleteDeviceIfExists(GetCowName(name), 4000ms)) {
        LOG(ERROR) << "Cannot unmap: " << GetCowName(name);
        return false;
    }

    std::string cow_image_name = GetCowImageDeviceName(name);
    if (!images_->UnmapImageIfExists(cow_image_name)) {
        LOG(ERROR) << "Cannot unmap image " << cow_image_name;
        return false;
    }
    return true;
}

bool SnapshotManager::UnmapDmUserDevice(const std::string& dm_user_name) {
    if (dm_.GetState(dm_user_name) == DmDeviceState::INVALID) {
        return true;
    }

    if (!DeleteDeviceIfExists(dm_user_name)) {
        LOG(ERROR) << "Cannot unmap " << dm_user_name;
        return false;
    }

    if (EnsureSnapuserdConnected()) {
        if (!snapuserd_client_->WaitForDeviceDelete(dm_user_name)) {
            LOG(ERROR) << "Failed to wait for " << dm_user_name << " control device to delete";
            return false;
        }
    }

    // Ensure the control device is gone so we don't run into ABA problems.
    auto control_device = "/dev/dm-user/" + dm_user_name;
    if (!android::fs_mgr::WaitForFileDeleted(control_device, 10s)) {
        LOG(ERROR) << "Timed out waiting for " << control_device << " to unlink";
        return false;
    }
    return true;
}

bool SnapshotManager::UnmapUserspaceSnapshotDevice(LockedFile* lock,
                                                   const std::string& snapshot_name) {
    auto dm_user_name = GetDmUserCowName(snapshot_name, GetSnapshotDriver(lock));
    if (dm_.GetState(dm_user_name) == DmDeviceState::INVALID) {
        return true;
    }

    CHECK(lock);

    SnapshotStatus snapshot_status;

    if (!ReadSnapshotStatus(lock, snapshot_name, &snapshot_status)) {
        return false;
    }
    // If the merge is complete, then we switch dm tables which is equivalent
    // to unmap; hence, we can't be deleting the device
    // as the table would be mounted off partitions and will fail.
    if (snapshot_status.state() != SnapshotState::MERGE_COMPLETED) {
        if (!DeleteDeviceIfExists(dm_user_name, 4000ms)) {
            LOG(ERROR) << "Cannot unmap " << dm_user_name;
            return false;
        }
    }

    if (EnsureSnapuserdConnected()) {
        if (!snapuserd_client_->WaitForDeviceDelete(dm_user_name)) {
            LOG(ERROR) << "Failed to wait for " << dm_user_name << " control device to delete";
            return false;
        }
    }

    // Ensure the control device is gone so we don't run into ABA problems.
    auto control_device = "/dev/dm-user/" + dm_user_name;
    if (!android::fs_mgr::WaitForFileDeleted(control_device, 10s)) {
        LOG(ERROR) << "Timed out waiting for " << control_device << " to unlink";
        return false;
    }
    return true;
}

bool SnapshotManager::MapAllSnapshots(const std::chrono::milliseconds& timeout_ms) {
    auto lock = LockExclusive();
    if (!lock) return false;

    auto state = ReadUpdateState(lock.get());
    if (state == UpdateState::Unverified) {
        if (GetCurrentSlot() == Slot::Target) {
            LOG(ERROR) << "Cannot call MapAllSnapshots when booting from the target slot.";
            return false;
        }
    } else if (state != UpdateState::Initiated) {
        LOG(ERROR) << "Cannot call MapAllSnapshots from update state: " << state;
        return false;
    }

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        return false;
    }

    const auto& opener = device_->GetPartitionOpener();
    auto slot_suffix = device_->GetOtherSlotSuffix();
    auto slot_number = SlotNumberForSlotSuffix(slot_suffix);
    auto super_device = device_->GetSuperDevice(slot_number);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot_number);
    if (!metadata) {
        LOG(ERROR) << "MapAllSnapshots could not read dynamic partition metadata for device: "
                   << super_device;
        return false;
    }

    for (const auto& snapshot : snapshots) {
        if (!UnmapPartitionWithSnapshot(lock.get(), snapshot)) {
            LOG(ERROR) << "MapAllSnapshots could not unmap snapshot: " << snapshot;
            return false;
        }

        CreateLogicalPartitionParams params = {
                .block_device = super_device,
                .metadata = metadata.get(),
                .partition_name = snapshot,
                .timeout_ms = timeout_ms,
                .partition_opener = &opener,
        };
        if (!MapPartitionWithSnapshot(lock.get(), std::move(params), SnapshotContext::Mount,
                                      nullptr)) {
            LOG(ERROR) << "MapAllSnapshots failed to map: " << snapshot;
            return false;
        }
    }

    LOG(INFO) << "MapAllSnapshots succeeded.";
    return true;
}

bool SnapshotManager::UnmapAllSnapshots() {
    auto lock = LockExclusive();
    if (!lock) return false;

    return UnmapAllSnapshots(lock.get());
}

bool SnapshotManager::UnmapAllSnapshots(LockedFile* lock) {
    LOG(INFO) << "Lock acquired for " << __FUNCTION__;
    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        return false;
    }
    LOG(INFO) << "Found " << snapshots.size() << " partitions with snapshots";

    for (const auto& snapshot : snapshots) {
        if (!UnmapPartitionWithSnapshot(lock, snapshot)) {
            LOG(ERROR) << "Failed to unmap snapshot: " << snapshot;
            return false;
        }
    }
    LOG(INFO) << "Unmapped " << snapshots.size() << " partitions with snapshots";

    // Terminate the daemon and release the snapuserd_client_ object.
    // If we need to re-connect with the daemon, EnsureSnapuserdConnected()
    // will re-create the object and establish the socket connection.
    if (snapuserd_client_) {
        LOG(INFO) << "Shutdown snapuserd daemon";
        snapuserd_client_->DetachSnapuserd();
        snapuserd_client_ = nullptr;
    }

    return true;
}

auto SnapshotManager::OpenFile(const std::string& file,
                               int lock_flags) -> std::unique_ptr<LockedFile> {
    const auto start = std::chrono::system_clock::now();
    unique_fd fd(open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd < 0) {
        PLOG(ERROR) << "Open failed: " << file;
        return nullptr;
    }
    if (lock_flags != 0 && TEMP_FAILURE_RETRY(flock(fd, lock_flags)) < 0) {
        PLOG(ERROR) << "Acquire flock failed: " << file;
        return nullptr;
    }
    // For simplicity, we want to CHECK that lock_mode == LOCK_EX, in some
    // calls, so strip extra flags.
    int lock_mode = lock_flags & (LOCK_EX | LOCK_SH);
    const auto end = std::chrono::system_clock::now();
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    if (duration_ms >= 1000ms) {
        LOG(INFO) << "Taking lock on " << file << " took " << duration_ms.count() << "ms";
    }
    return std::make_unique<LockedFile>(file, std::move(fd), lock_mode);
}

SnapshotManager::LockedFile::~LockedFile() {
    if (TEMP_FAILURE_RETRY(flock(fd_, LOCK_UN)) < 0) {
        PLOG(ERROR) << "Failed to unlock file: " << path_;
    }
}

std::string SnapshotManager::GetStateFilePath() const {
    return metadata_dir_ + "/state"s;
}

std::string SnapshotManager::GetMergeStateFilePath() const {
    return metadata_dir_ + "/merge_state"s;
}

std::string SnapshotManager::GetLockPath() const {
    return metadata_dir_;
}

std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::OpenLock(int lock_flags) {
    auto lock_file = GetLockPath();
    return OpenFile(lock_file, lock_flags);
}

std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::LockShared() {
    return OpenLock(LOCK_SH);
}

std::unique_ptr<SnapshotManager::LockedFile> SnapshotManager::LockExclusive() {
    return OpenLock(LOCK_EX);
}

static UpdateState UpdateStateFromString(const std::string& contents) {
    if (contents.empty() || contents == "none") {
        return UpdateState::None;
    } else if (contents == "initiated") {
        return UpdateState::Initiated;
    } else if (contents == "unverified") {
        return UpdateState::Unverified;
    } else if (contents == "merging") {
        return UpdateState::Merging;
    } else if (contents == "merge-completed") {
        return UpdateState::MergeCompleted;
    } else if (contents == "merge-needs-reboot") {
        return UpdateState::MergeNeedsReboot;
    } else if (contents == "merge-failed") {
        return UpdateState::MergeFailed;
    } else if (contents == "cancelled") {
        return UpdateState::Cancelled;
    } else {
        LOG(ERROR) << "Unknown merge state in update state file: \"" << contents << "\"";
        return UpdateState::None;
    }
}

std::ostream& operator<<(std::ostream& os, UpdateState state) {
    switch (state) {
        case UpdateState::None:
            return os << "none";
        case UpdateState::Initiated:
            return os << "initiated";
        case UpdateState::Unverified:
            return os << "unverified";
        case UpdateState::Merging:
            return os << "merging";
        case UpdateState::MergeCompleted:
            return os << "merge-completed";
        case UpdateState::MergeNeedsReboot:
            return os << "merge-needs-reboot";
        case UpdateState::MergeFailed:
            return os << "merge-failed";
        case UpdateState::Cancelled:
            return os << "cancelled";
        default:
            LOG(ERROR) << "Unknown update state: " << static_cast<uint32_t>(state);
            return os;
    }
}

std::ostream& operator<<(std::ostream& os, MergePhase phase) {
    switch (phase) {
        case MergePhase::NO_MERGE:
            return os << "none";
        case MergePhase::FIRST_PHASE:
            return os << "first";
        case MergePhase::SECOND_PHASE:
            return os << "second";
        default:
            LOG(ERROR) << "Unknown merge phase: " << static_cast<uint32_t>(phase);
            return os << "unknown(" << static_cast<uint32_t>(phase) << ")";
    }
}

UpdateState SnapshotManager::ReadUpdateState(LockedFile* lock) {
    SnapshotUpdateStatus status = ReadSnapshotUpdateStatus(lock);
    return status.state();
}

SnapshotUpdateStatus SnapshotManager::ReadSnapshotUpdateStatus(LockedFile* lock) {
    CHECK(lock);

    SnapshotUpdateStatus status = {};
    std::string contents;
    if (!android::base::ReadFileToString(GetStateFilePath(), &contents)) {
        PLOG(ERROR) << "Read state file failed";
        status.set_state(UpdateState::None);
        return status;
    }

    if (!status.ParseFromString(contents)) {
        LOG(WARNING) << "Unable to parse state file as SnapshotUpdateStatus, using the old format";

        // Try to rollback to legacy file to support devices that are
        // currently using the old file format.
        // TODO(b/147409432)
        status.set_state(UpdateStateFromString(contents));
    }

    return status;
}

bool SnapshotManager::WriteUpdateState(LockedFile* lock, UpdateState state,
                                       MergeFailureCode failure_code) {
    SnapshotUpdateStatus status;
    status.set_state(state);

    switch (state) {
        case UpdateState::MergeFailed:
            status.set_merge_failure_code(failure_code);
            break;
        case UpdateState::Initiated:
            status.set_source_build_fingerprint(
                    android::base::GetProperty("ro.build.fingerprint", ""));
            break;
        default:
            break;
    }

    // If we're transitioning between two valid states (eg, we're not beginning
    // or ending an OTA), then make sure to propagate the compression bit and
    // build fingerprint.
    if (!(state == UpdateState::Initiated || state == UpdateState::None)) {
        SnapshotUpdateStatus old_status = ReadSnapshotUpdateStatus(lock);
        status.set_using_snapuserd(old_status.using_snapuserd());
        status.set_source_build_fingerprint(old_status.source_build_fingerprint());
        status.set_merge_phase(old_status.merge_phase());
        status.set_userspace_snapshots(old_status.userspace_snapshots());
        status.set_io_uring_enabled(old_status.io_uring_enabled());
        status.set_legacy_snapuserd(old_status.legacy_snapuserd());
        status.set_o_direct(old_status.o_direct());
        status.set_skip_verification(old_status.skip_verification());
        status.set_cow_op_merge_size(old_status.cow_op_merge_size());
        status.set_num_worker_threads(old_status.num_worker_threads());
        status.set_verify_block_size(old_status.verify_block_size());
        status.set_num_verification_threads(old_status.num_verification_threads());
    }
    return WriteSnapshotUpdateStatus(lock, status);
}

bool SnapshotManager::WriteSnapshotUpdateStatus(LockedFile* lock,
                                                const SnapshotUpdateStatus& status) {
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);

    std::string contents;
    if (!status.SerializeToString(&contents)) {
        LOG(ERROR) << "Unable to serialize SnapshotUpdateStatus.";
        return false;
    }

#ifdef LIBSNAPSHOT_USE_HAL
    auto merge_status = MergeStatus::UNKNOWN;
    switch (status.state()) {
        // The needs-reboot and completed cases imply that /data and /metadata
        // can be safely wiped, so we don't report a merge status.
        case UpdateState::None:
        case UpdateState::MergeNeedsReboot:
        case UpdateState::MergeCompleted:
        case UpdateState::Initiated:
            merge_status = MergeStatus::NONE;
            break;
        case UpdateState::Unverified:
            merge_status = MergeStatus::SNAPSHOTTED;
            break;
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            merge_status = MergeStatus::MERGING;
            break;
        default:
            // Note that Cancelled flows to here - it is never written, since
            // it only communicates a transient state to the caller.
            LOG(ERROR) << "Unexpected update status: " << status.state();
            break;
    }

    bool set_before_write =
            merge_status == MergeStatus::SNAPSHOTTED || merge_status == MergeStatus::MERGING;
    if (set_before_write && !device_->SetBootControlMergeStatus(merge_status)) {
        return false;
    }
#endif

    if (!WriteStringToFileAtomic(contents, GetStateFilePath())) {
        PLOG(ERROR) << "Could not write to state file";
        return false;
    }

#ifdef LIBSNAPSHOT_USE_HAL
    if (!set_before_write && !device_->SetBootControlMergeStatus(merge_status)) {
        return false;
    }
#endif
    return true;
}

std::string SnapshotManager::GetSnapshotStatusFilePath(const std::string& name) {
    auto file = metadata_dir_ + "/snapshots/"s + name;
    return file;
}

bool SnapshotManager::ReadSnapshotStatus(LockedFile* lock, const std::string& name,
                                         SnapshotStatus* status) {
    CHECK(lock);
    auto path = GetSnapshotStatusFilePath(name);

    unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd < 0) {
        PLOG(ERROR) << "Open failed: " << path;
        return false;
    }

    if (!status->ParseFromFileDescriptor(fd.get())) {
        PLOG(ERROR) << "Unable to parse " << path << " as SnapshotStatus";
        return false;
    }

    if (status->name() != name) {
        LOG(WARNING) << "Found snapshot status named " << status->name() << " in " << path;
        status->set_name(name);
    }

    return true;
}

bool SnapshotManager::WriteSnapshotStatus(LockedFile* lock, const SnapshotStatus& status) {
    // The caller must take an exclusive lock to modify snapshots.
    CHECK(lock);
    CHECK(lock->lock_mode() == LOCK_EX);
    CHECK(!status.name().empty());

    auto path = GetSnapshotStatusFilePath(status.name());

    std::string content;
    if (!status.SerializeToString(&content)) {
        LOG(ERROR) << "Unable to serialize SnapshotStatus for " << status.name();
        return false;
    }

    if (!WriteStringToFileAtomic(content, path)) {
        PLOG(ERROR) << "Unable to write SnapshotStatus to " << path;
        return false;
    }

    return true;
}

bool SnapshotManager::EnsureImageManager() {
    if (images_) return true;

    images_ = device_->OpenImageManager();
    if (!images_) {
        LOG(ERROR) << "Could not open ImageManager";
        return false;
    }
    return true;
}

bool SnapshotManager::EnsureSnapuserdConnected(std::chrono::milliseconds timeout_ms) {
    if (snapuserd_client_) {
        return true;
    }

    if (!use_first_stage_snapuserd_ && !EnsureSnapuserdStarted()) {
        return false;
    }

    snapuserd_client_ = SnapuserdClient::Connect(kSnapuserdSocket, timeout_ms);
    if (!snapuserd_client_) {
        LOG(ERROR) << "Unable to connect to snapuserd";
        return false;
    }
    return true;
}

void SnapshotManager::UnmapAndDeleteCowPartition(MetadataBuilder* current_metadata) {
    std::vector<std::string> to_delete;
    for (auto* existing_cow_partition : current_metadata->ListPartitionsInGroup(kCowGroupName)) {
        if (!DeleteDeviceIfExists(existing_cow_partition->name())) {
            LOG(WARNING) << existing_cow_partition->name()
                         << " cannot be unmapped and its space cannot be reclaimed";
            continue;
        }
        to_delete.push_back(existing_cow_partition->name());
    }
    for (const auto& name : to_delete) {
        current_metadata->RemovePartition(name);
    }
}

static Return AddRequiredSpace(Return orig,
                               const std::map<std::string, SnapshotStatus>& all_snapshot_status) {
    if (orig.error_code() != Return::ErrorCode::NO_SPACE) {
        return orig;
    }
    uint64_t sum = 0;
    for (auto&& [name, status] : all_snapshot_status) {
        sum += status.cow_file_size();
    }
    LOG(INFO) << "Calculated needed COW space: " << sum << " bytes";
    return Return::NoSpace(sum);
}

Return SnapshotManager::CreateUpdateSnapshots(const DeltaArchiveManifest& manifest) {
    auto lock = LockExclusive();
    if (!lock) return Return::Error();

    auto update_state = ReadUpdateState(lock.get());
    if (update_state != UpdateState::Initiated) {
        LOG(ERROR) << "Cannot create update snapshots in state " << update_state;
        return Return::Error();
    }

    // TODO(b/134949511): remove this check. Right now, with overlayfs mounted, the scratch
    // partition takes up a big chunk of space in super, causing COW images to be created on
    // retrofit Virtual A/B devices.
    if (device_->IsOverlayfsSetup()) {
        LOG(ERROR) << "Cannot create update snapshots with overlayfs setup. Run `adb enable-verity`"
                   << ", reboot, then try again.";
        return Return::Error();
    }

    const auto& opener = device_->GetPartitionOpener();
    auto current_suffix = device_->GetSlotSuffix();
    uint32_t current_slot = SlotNumberForSlotSuffix(current_suffix);
    auto target_suffix = device_->GetOtherSlotSuffix();
    uint32_t target_slot = SlotNumberForSlotSuffix(target_suffix);
    auto current_super = device_->GetSuperDevice(current_slot);

    auto current_metadata = MetadataBuilder::New(opener, current_super, current_slot);
    if (current_metadata == nullptr) {
        LOG(ERROR) << "Cannot create metadata builder.";
        return Return::Error();
    }

    auto target_metadata =
            MetadataBuilder::NewForUpdate(opener, current_super, current_slot, target_slot);
    if (target_metadata == nullptr) {
        LOG(ERROR) << "Cannot create target metadata builder.";
        return Return::Error();
    }

    // Delete partitions with target suffix in |current_metadata|. Otherwise,
    // partition_cow_creator recognizes these left-over partitions as used space.
    for (const auto& group_name : current_metadata->ListGroups()) {
        if (android::base::EndsWith(group_name, target_suffix)) {
            current_metadata->RemoveGroupAndPartitions(group_name);
        }
    }

    SnapshotMetadataUpdater metadata_updater(target_metadata.get(), target_slot, manifest);
    if (!metadata_updater.Update()) {
        LOG(ERROR) << "Cannot calculate new metadata.";
        return Return::Error();
    }

    // Delete previous COW partitions in current_metadata so that PartitionCowCreator marks those as
    // free regions.
    UnmapAndDeleteCowPartition(current_metadata.get());

    // Check that all these metadata is not retrofit dynamic partitions. Snapshots on
    // devices with retrofit dynamic partitions does not make sense.
    // This ensures that current_metadata->GetFreeRegions() uses the same device
    // indices as target_metadata (i.e. 0 -> "super").
    // This is also assumed in MapCowDevices() call below.
    CHECK(current_metadata->GetBlockDevicePartitionName(0) == LP_METADATA_DEFAULT_PARTITION_NAME &&
          target_metadata->GetBlockDevicePartitionName(0) == LP_METADATA_DEFAULT_PARTITION_NAME);

    const auto& dap_metadata = manifest.dynamic_partition_metadata();

    std::string vabc_disable_reason;
    if (!dap_metadata.vabc_enabled()) {
        vabc_disable_reason = "not enabled metadata";
    } else if (device_->IsRecovery()) {
        vabc_disable_reason = "recovery";
    } else if (!KernelSupportsCompressedSnapshots()) {
        vabc_disable_reason = "kernel missing userspace block device support";
    }

    // Deduce supported features.
    bool userspace_snapshots = CanUseUserspaceSnapshots();
    bool legacy_compression = GetLegacyCompressionEnabledProperty();
    bool is_legacy_snapuserd = IsVendorFromAndroid12();

    if (!vabc_disable_reason.empty()) {
        if (userspace_snapshots) {
            LOG(INFO) << "Userspace snapshots disabled: " << vabc_disable_reason;
        }
        if (legacy_compression) {
            LOG(INFO) << "Compression disabled: " << vabc_disable_reason;
        }
        userspace_snapshots = false;
        legacy_compression = false;
        is_legacy_snapuserd = false;
    }

    if (legacy_compression || userspace_snapshots) {
        if (dap_metadata.cow_version() < kMinCowVersion ||
            dap_metadata.cow_version() > kMaxCowVersion) {
            LOG(ERROR) << "Manifest cow version is out of bounds (got: "
                       << dap_metadata.cow_version() << ", min: " << kMinCowVersion
                       << ", max: " << kMaxCowVersion << ")";
            return Return::Error();
        }
    }

    if (!userspace_snapshots && is_legacy_snapuserd && legacy_compression) {
        userspace_snapshots = true;
        LOG(INFO) << "Vendor from Android 12. Enabling userspace snapshot for OTA install";
    }

    const bool using_snapuserd = userspace_snapshots || legacy_compression;
    if (!using_snapuserd) {
        LOG(INFO) << "Using legacy Virtual A/B (dm-snapshot)";
    }

    std::string compression_algorithm;
    uint64_t compression_factor{};
    if (using_snapuserd) {
        compression_algorithm = dap_metadata.vabc_compression_param();
        compression_factor = dap_metadata.compression_factor();
        if (compression_algorithm.empty()) {
            // Older OTAs don't set an explicit compression type, so default to gz.
            compression_algorithm = "gz";
        }
        LOG(INFO) << "using compression algorithm: " << compression_algorithm
                  << ", max compressible block size: " << compression_factor;
    }
    auto read_ahead_size =
            android::base::GetUintProperty<uint>("ro.virtual_ab.read_ahead_size", kReadAheadSizeKb);
    PartitionCowCreator cow_creator{
            .target_metadata = target_metadata.get(),
            .target_suffix = target_suffix,
            .target_partition = nullptr,
            .current_metadata = current_metadata.get(),
            .current_suffix = current_suffix,
            .update = nullptr,
            .extra_extents = {},
            .using_snapuserd = using_snapuserd,
            .compression_algorithm = compression_algorithm,
            .compression_factor = compression_factor,
            .read_ahead_size = read_ahead_size,
    };

    if (dap_metadata.vabc_feature_set().has_threaded()) {
        cow_creator.enable_threading = dap_metadata.vabc_feature_set().threaded();
    }
    if (dap_metadata.vabc_feature_set().has_batch_writes()) {
        cow_creator.batched_writes = dap_metadata.vabc_feature_set().batch_writes();
    }

    // In case of error, automatically delete devices that are created along the way.
    // Note that "lock" is destroyed after "created_devices", so it is safe to use |lock| for
    // these devices.
    AutoDeviceList created_devices;
    std::map<std::string, SnapshotStatus> all_snapshot_status;
    auto ret = CreateUpdateSnapshotsInternal(lock.get(), manifest, &cow_creator, &created_devices,
                                             &all_snapshot_status);
    if (!ret.is_ok()) {
        LOG(ERROR) << "CreateUpdateSnapshotsInternal failed: " << ret.string();
        return ret;
    }

    auto exported_target_metadata = target_metadata->Export();
    if (exported_target_metadata == nullptr) {
        LOG(ERROR) << "Cannot export target metadata";
        return Return::Error();
    }

    ret = InitializeUpdateSnapshots(lock.get(), dap_metadata.cow_version(), target_metadata.get(),
                                    exported_target_metadata.get(), target_suffix,
                                    all_snapshot_status);
    if (!ret.is_ok()) return ret;

    if (!UpdatePartitionTable(opener, device_->GetSuperDevice(target_slot),
                              *exported_target_metadata, target_slot)) {
        LOG(ERROR) << "Cannot write target metadata";
        return Return::Error();
    }

    // If snapuserd is enabled, we need to retain a copy of the old metadata
    // so we can access original blocks in case they are moved around. We do
    // not want to rely on the old super metadata slot because we don't
    // guarantee its validity after the slot switch is successful.
    if (using_snapuserd) {
        auto metadata = current_metadata->Export();
        if (!metadata) {
            LOG(ERROR) << "Could not export current metadata";
            return Return::Error();
        }

        auto path = GetOldPartitionMetadataPath();
        if (!android::fs_mgr::WriteToImageFile(path, *metadata.get())) {
            LOG(ERROR) << "Cannot write old metadata to " << path;
            return Return::Error();
        }
    }

    SnapshotUpdateStatus status = ReadSnapshotUpdateStatus(lock.get());
    status.set_state(update_state);
    status.set_using_snapuserd(using_snapuserd);

    if (userspace_snapshots) {
        status.set_userspace_snapshots(true);
        LOG(INFO) << "Virtual A/B using userspace snapshots";

        if (GetIouringEnabledProperty()) {
            status.set_io_uring_enabled(true);
            LOG(INFO) << "io_uring for snapshots enabled";
        }
        if (GetODirectEnabledProperty()) {
            status.set_o_direct(true);
            LOG(INFO) << "o_direct for source image enabled";
        }
        if (GetSkipVerificationProperty()) {
            status.set_skip_verification(true);
            LOG(INFO) << "skipping verification of images";
        }
        if (is_legacy_snapuserd) {
            status.set_legacy_snapuserd(true);
            LOG(INFO) << "Setting legacy_snapuserd to true";
        }
        status.set_cow_op_merge_size(
                android::base::GetUintProperty<uint32_t>("ro.virtual_ab.cow_op_merge_size", 0));
        status.set_num_worker_threads(
                android::base::GetUintProperty<uint32_t>("ro.virtual_ab.num_worker_threads", 0));
        status.set_verify_block_size(
                android::base::GetUintProperty<uint32_t>("ro.virtual_ab.verify_block_size", 0));
        status.set_num_verification_threads(
                android::base::GetUintProperty<uint32_t>("ro.virtual_ab.num_verify_threads", 0));
    } else if (legacy_compression) {
        LOG(INFO) << "Virtual A/B using legacy snapuserd";
    } else {
        LOG(INFO) << "Virtual A/B using dm-snapshot";
    }

    is_snapshot_userspace_.emplace(userspace_snapshots);
    is_legacy_snapuserd_.emplace(is_legacy_snapuserd);

    if (!device()->IsTestDevice() && using_snapuserd) {
        // Terminate stale daemon if any
        std::unique_ptr<SnapuserdClient> snapuserd_client = std::move(snapuserd_client_);
        if (!snapuserd_client) {
            snapuserd_client = SnapuserdClient::TryConnect(kSnapuserdSocket, 5s);
        }
        if (snapuserd_client) {
            snapuserd_client->DetachSnapuserd();
            snapuserd_client = nullptr;
        }
    }

    if (!WriteSnapshotUpdateStatus(lock.get(), status)) {
        LOG(ERROR) << "Unable to write new update state";
        return Return::Error();
    }

    created_devices.Release();
    LOG(INFO) << "Successfully created all snapshots for target slot " << target_suffix;

    return Return::Ok();
}

Return SnapshotManager::CreateUpdateSnapshotsInternal(
        LockedFile* lock, const DeltaArchiveManifest& manifest, PartitionCowCreator* cow_creator,
        AutoDeviceList* created_devices,
        std::map<std::string, SnapshotStatus>* all_snapshot_status) {
    CHECK(lock);

    auto* target_metadata = cow_creator->target_metadata;
    const auto& target_suffix = cow_creator->target_suffix;

    if (!target_metadata->AddGroup(kCowGroupName, 0)) {
        LOG(ERROR) << "Cannot add group " << kCowGroupName;
        return Return::Error();
    }

    std::map<std::string, const PartitionUpdate*> partition_map;
    std::map<std::string, std::vector<Extent>> extra_extents_map;
    for (const auto& partition_update : manifest.partitions()) {
        auto suffixed_name = partition_update.partition_name() + target_suffix;
        auto&& [it, inserted] = partition_map.emplace(suffixed_name, &partition_update);
        if (!inserted) {
            LOG(ERROR) << "Duplicated partition " << partition_update.partition_name()
                       << " in update manifest.";
            return Return::Error();
        }

        auto& extra_extents = extra_extents_map[suffixed_name];
        if (partition_update.has_hash_tree_extent()) {
            extra_extents.push_back(partition_update.hash_tree_extent());
        }
        if (partition_update.has_fec_extent()) {
            extra_extents.push_back(partition_update.fec_extent());
        }
    }

    for (auto* target_partition : ListPartitionsWithSuffix(target_metadata, target_suffix)) {
        cow_creator->target_partition = target_partition;
        cow_creator->update = nullptr;
        auto iter = partition_map.find(target_partition->name());
        if (iter != partition_map.end()) {
            cow_creator->update = iter->second;
        } else {
            LOG(INFO) << target_partition->name()
                      << " isn't included in the payload, skipping the cow creation.";
            continue;
        }

        cow_creator->extra_extents.clear();
        auto extra_extents_it = extra_extents_map.find(target_partition->name());
        if (extra_extents_it != extra_extents_map.end()) {
            cow_creator->extra_extents = std::move(extra_extents_it->second);
        }

        // Compute the device sizes for the partition.
        auto cow_creator_ret = cow_creator->Run();
        if (!cow_creator_ret.has_value()) {
            LOG(ERROR) << "PartitionCowCreator returned no value for " << target_partition->name();
            return Return::Error();
        }

        LOG(INFO) << "For partition " << target_partition->name()
                  << ", device size = " << cow_creator_ret->snapshot_status.device_size()
                  << ", snapshot size = " << cow_creator_ret->snapshot_status.snapshot_size()
                  << ", cow partition size = "
                  << cow_creator_ret->snapshot_status.cow_partition_size()
                  << ", cow file size = " << cow_creator_ret->snapshot_status.cow_file_size();

        // Delete any existing snapshot before re-creating one.
        if (!DeleteSnapshot(lock, target_partition->name())) {
            LOG(ERROR) << "Cannot delete existing snapshot before creating a new one for partition "
                       << target_partition->name();
            return Return::Error();
        }

        // It is possible that the whole partition uses free space in super, and snapshot / COW
        // would not be needed. In this case, skip the partition.
        bool needs_snapshot = cow_creator_ret->snapshot_status.snapshot_size() > 0;
        bool needs_cow = (cow_creator_ret->snapshot_status.cow_partition_size() +
                          cow_creator_ret->snapshot_status.cow_file_size()) > 0;
        CHECK(needs_snapshot == needs_cow);

        if (!needs_snapshot) {
            LOG(INFO) << "Skip creating snapshot for partition " << target_partition->name()
                      << "because nothing needs to be snapshotted.";
            continue;
        }

        // Find the original partition size.
        auto name = target_partition->name();
        auto old_partition_name =
                name.substr(0, name.size() - target_suffix.size()) + cow_creator->current_suffix;
        auto old_partition = cow_creator->current_metadata->FindPartition(old_partition_name);
        if (old_partition) {
            cow_creator_ret->snapshot_status.set_old_partition_size(old_partition->size());
        }

        // Store these device sizes to snapshot status file.
        if (!CreateSnapshot(lock, cow_creator, &cow_creator_ret->snapshot_status)) {
            return Return::Error();
        }
        created_devices->EmplaceBack<AutoDeleteSnapshot>(this, lock, target_partition->name());

        // Create the COW partition. That is, use any remaining free space in super partition before
        // creating the COW images.
        if (cow_creator_ret->snapshot_status.cow_partition_size() > 0) {
            CHECK(cow_creator_ret->snapshot_status.cow_partition_size() % kSectorSize == 0)
                    << "cow_partition_size == "
                    << cow_creator_ret->snapshot_status.cow_partition_size()
                    << " is not a multiple of sector size " << kSectorSize;
            auto cow_partition = target_metadata->AddPartition(GetCowName(target_partition->name()),
                                                               kCowGroupName, 0 /* flags */);
            if (cow_partition == nullptr) {
                return Return::Error();
            }

            if (!target_metadata->ResizePartition(
                        cow_partition, cow_creator_ret->snapshot_status.cow_partition_size(),
                        cow_creator_ret->cow_partition_usable_regions)) {
                LOG(ERROR) << "Cannot create COW partition on metadata with size "
                           << cow_creator_ret->snapshot_status.cow_partition_size();
                return Return::Error();
            }
            // Only the in-memory target_metadata is modified; nothing to clean up if there is an
            // error in the future.
        }

        all_snapshot_status->emplace(target_partition->name(),
                                     std::move(cow_creator_ret->snapshot_status));

        LOG(INFO) << "Successfully created snapshot partition for " << target_partition->name();
    }

    LOG(INFO) << "Allocating CoW images.";

    for (auto&& [name, snapshot_status] : *all_snapshot_status) {
        // Create the backing COW image if necessary.
        if (snapshot_status.cow_file_size() > 0) {
            auto ret = CreateCowImage(lock, name);
            if (!ret.is_ok()) {
                LOG(ERROR) << "CreateCowImage failed: " << ret.string();
                return AddRequiredSpace(ret, *all_snapshot_status);
            }
        }

        LOG(INFO) << "Successfully created snapshot for " << name;
    }

    return Return::Ok();
}

Return SnapshotManager::InitializeUpdateSnapshots(
        LockedFile* lock, uint32_t cow_version, MetadataBuilder* target_metadata,
        const LpMetadata* exported_target_metadata, const std::string& target_suffix,
        const std::map<std::string, SnapshotStatus>& all_snapshot_status) {
    CHECK(lock);

    CreateLogicalPartitionParams cow_params{
            .block_device = LP_METADATA_DEFAULT_PARTITION_NAME,
            .metadata = exported_target_metadata,
            .timeout_ms = std::chrono::milliseconds::max(),
            .partition_opener = &device_->GetPartitionOpener(),
    };
    for (auto* target_partition : ListPartitionsWithSuffix(target_metadata, target_suffix)) {
        AutoDeviceList created_devices_for_cow;

        if (!UnmapPartitionWithSnapshot(lock, target_partition->name())) {
            LOG(ERROR) << "Cannot unmap existing COW devices before re-mapping them for zero-fill: "
                       << target_partition->name();
            return Return::Error();
        }

        auto it = all_snapshot_status.find(target_partition->name());
        if (it == all_snapshot_status.end()) continue;
        cow_params.partition_name = target_partition->name();
        std::string cow_name;
        if (!MapCowDevices(lock, cow_params, it->second, &created_devices_for_cow, &cow_name)) {
            return Return::Error();
        }

        std::string cow_path;
        if (!images_->GetMappedImageDevice(cow_name, &cow_path)) {
            LOG(ERROR) << "Cannot determine path for " << cow_name;
            return Return::Error();
        }

        if (!android::fs_mgr::WaitForFile(cow_path, 6s)) {
            LOG(ERROR) << "Timed out waiting for device to appear: " << cow_path;
            return Return::Error();
        }

        if (it->second.using_snapuserd()) {
            unique_fd fd(open(cow_path.c_str(), O_RDWR | O_CLOEXEC));
            if (fd < 0) {
                PLOG(ERROR) << "open " << cow_path << " failed for snapshot "
                            << cow_params.partition_name;
                return Return::Error();
            }

            CowOptions options;
            if (device()->IsTestDevice()) {
                options.scratch_space = false;
            }
            options.compression = it->second.compression_algorithm();
            if (cow_version >= 3) {
                options.op_count_max = it->second.estimated_ops_buffer_size();
                options.max_blocks = {it->second.device_size() / options.block_size};
            }

            auto writer = CreateCowWriter(cow_version, options, std::move(fd));
            if (!writer->Finalize()) {
                LOG(ERROR) << "Could not initialize COW device for " << target_partition->name();
                return Return::Error();
            }
        } else {
            auto ret = InitializeKernelCow(cow_path);
            if (!ret.is_ok()) {
                LOG(ERROR) << "Can't zero-fill COW device for " << target_partition->name() << ": "
                           << cow_path;
                return AddRequiredSpace(ret, all_snapshot_status);
            }
        }
        // Let destructor of created_devices_for_cow to unmap the COW devices.
    };
    return Return::Ok();
}

bool SnapshotManager::MapUpdateSnapshot(const CreateLogicalPartitionParams& params,
                                        std::string* snapshot_path) {
    auto lock = LockShared();
    if (!lock) return false;
    if (!UnmapPartitionWithSnapshot(lock.get(), params.GetPartitionName())) {
        LOG(ERROR) << "Cannot unmap existing snapshot before re-mapping it: "
                   << params.GetPartitionName();
        return false;
    }

    SnapshotStatus status;
    if (!ReadSnapshotStatus(lock.get(), params.GetPartitionName(), &status)) {
        return false;
    }
    if (status.using_snapuserd()) {
        LOG(ERROR) << "Cannot use MapUpdateSnapshot with snapuserd";
        return false;
    }

    SnapshotPaths paths;
    if (!MapPartitionWithSnapshot(lock.get(), params, SnapshotContext::Update, &paths)) {
        return false;
    }

    if (!paths.snapshot_device.empty()) {
        *snapshot_path = paths.snapshot_device;
    } else {
        *snapshot_path = paths.target_device;
    }
    DCHECK(!snapshot_path->empty());
    return true;
}

std::unique_ptr<ICowWriter> SnapshotManager::OpenSnapshotWriter(
        const android::fs_mgr::CreateLogicalPartitionParams& params,
        std::optional<uint64_t> label) {
#if defined(LIBSNAPSHOT_NO_COW_WRITE)
    (void)params;
    (void)label;

    LOG(ERROR) << "Snapshots cannot be written in first-stage init or recovery";
    return nullptr;
#else
    // First unmap any existing mapping.
    auto lock = LockShared();
    if (!lock) return nullptr;
    if (!UnmapPartitionWithSnapshot(lock.get(), params.GetPartitionName())) {
        LOG(ERROR) << "Cannot unmap existing snapshot before re-mapping it: "
                   << params.GetPartitionName();
        return nullptr;
    }

    SnapshotPaths paths;
    if (!MapPartitionWithSnapshot(lock.get(), params, SnapshotContext::Update, &paths)) {
        return nullptr;
    }

    SnapshotStatus status;
    if (!paths.cow_device_name.empty()) {
        if (!ReadSnapshotStatus(lock.get(), params.GetPartitionName(), &status)) {
            return nullptr;
        }
    } else {
        // Currently, partition_cow_creator always creates snapshots. The
        // reason is that if partition X shrinks while partition Y grows, we
        // cannot bindly write to the newly freed extents in X. This would
        // make the old slot unusable. So, the entire size of the target
        // partition is currently considered snapshottable.
        LOG(ERROR) << "No snapshot available for partition " << params.GetPartitionName();
        return nullptr;
    }

    if (!status.using_snapuserd()) {
        LOG(ERROR) << "Can only create snapshot writers with userspace or compressed snapshots";
        return nullptr;
    }

    return OpenCompressedSnapshotWriter(lock.get(), status, paths, label);
#endif
}

#if !defined(LIBSNAPSHOT_NO_COW_WRITE)
std::unique_ptr<ICowWriter> SnapshotManager::OpenCompressedSnapshotWriter(
        LockedFile* lock, const SnapshotStatus& status, const SnapshotPaths& paths,
        std::optional<uint64_t> label) {
    CHECK(lock);

    CowOptions cow_options;
    cow_options.compression = status.compression_algorithm();
    cow_options.max_blocks = {status.device_size() / cow_options.block_size};
    cow_options.batch_write = status.batched_writes();
    cow_options.num_compress_threads = status.enable_threading() ? 2 : 1;
    cow_options.op_count_max = status.estimated_ops_buffer_size();
    cow_options.compression_factor = status.compression_factor();
    // Disable scratch space for vts tests
    if (device()->IsTestDevice()) {
        cow_options.scratch_space = false;
    }

    // Currently we don't support partial snapshots, since partition_cow_creator
    // never creates this scenario.
    CHECK(status.snapshot_size() == status.device_size());

    std::string cow_path;
    if (!GetMappedImageDevicePath(paths.cow_device_name, &cow_path)) {
        LOG(ERROR) << "Could not determine path for " << paths.cow_device_name;
        return nullptr;
    }

    unique_fd cow_fd(open(cow_path.c_str(), O_RDWR | O_CLOEXEC));
    if (cow_fd < 0) {
        PLOG(ERROR) << "OpenCompressedSnapshotWriter: open " << cow_path;
        return nullptr;
    }

    CowHeaderV3 header;
    if (!ReadCowHeader(cow_fd, &header)) {
        LOG(ERROR) << "OpenCompressedSnapshotWriter: read header failed";
        return nullptr;
    }

    return CreateCowWriter(header.prefix.major_version, cow_options, std::move(cow_fd), label);
}
#endif  // !defined(LIBSNAPSHOT_NO_COW_WRITE)

bool SnapshotManager::UnmapUpdateSnapshot(const std::string& target_partition_name) {
    auto lock = LockShared();
    if (!lock) return false;
    return UnmapPartitionWithSnapshot(lock.get(), target_partition_name);
}

bool SnapshotManager::UnmapAllPartitionsInRecovery() {
    auto lock = LockExclusive();
    if (!lock) return false;

    const auto& opener = device_->GetPartitionOpener();
    uint32_t slot = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto super_device = device_->GetSuperDevice(slot);
    auto metadata = android::fs_mgr::ReadMetadata(opener, super_device, slot);
    if (!metadata) {
        LOG(ERROR) << "Could not read dynamic partition metadata for device: " << super_device;
        return false;
    }

    bool ok = true;
    for (const auto& partition : metadata->partitions) {
        auto partition_name = GetPartitionName(partition);
        ok &= UnmapPartitionWithSnapshot(lock.get(), partition_name);
    }
    return ok;
}

std::ostream& operator<<(std::ostream& os, SnapshotManager::Slot slot) {
    switch (slot) {
        case SnapshotManager::Slot::Unknown:
            return os << "unknown";
        case SnapshotManager::Slot::Source:
            return os << "source";
        case SnapshotManager::Slot::Target:
            return os << "target";
    }
}

bool SnapshotManager::Dump(std::ostream& os) {
    // Don't actually lock. Dump() is for debugging purposes only, so it is okay
    // if it is racy.
    auto file = OpenLock(0 /* lock flag */);
    if (!file) return false;

    std::stringstream ss;

    auto update_status = ReadSnapshotUpdateStatus(file.get());

    ss << "Update state: " << update_status.state() << std::endl;
    ss << "Using snapuserd: " << update_status.using_snapuserd() << std::endl;
    ss << "Using userspace snapshots: " << update_status.userspace_snapshots() << std::endl;
    ss << "Using io_uring: " << update_status.io_uring_enabled() << std::endl;
    ss << "Using o_direct: " << update_status.o_direct() << std::endl;
    ss << "Using skip_verification: " << update_status.skip_verification() << std::endl;
    ss << "Cow op merge size (0 for uncapped): " << update_status.cow_op_merge_size() << std::endl;
    ss << "Worker thread count: " << update_status.num_worker_threads() << std::endl;
    ss << "Num verification threads: " << update_status.num_verification_threads() << std::endl;
    ss << "Verify block size: " << update_status.verify_block_size() << std::endl;
    ss << "Using XOR compression: " << GetXorCompressionEnabledProperty() << std::endl;
    ss << "Current slot: " << device_->GetSlotSuffix() << std::endl;
    ss << "Boot indicator: booting from " << GetCurrentSlot() << " slot" << std::endl;
    ss << "Rollback indicator: "
       << (access(GetRollbackIndicatorPath().c_str(), F_OK) == 0 ? "exists" : strerror(errno))
       << std::endl;
    ss << "Forward merge indicator: "
       << (access(GetForwardMergeIndicatorPath().c_str(), F_OK) == 0 ? "exists" : strerror(errno))
       << std::endl;
    ss << "Source build fingerprint: " << update_status.source_build_fingerprint() << std::endl;

    if (update_status.state() == UpdateState::Merging) {
        ss << "Merge completion: ";
        if (!EnsureSnapuserdConnected()) {
            ss << "N/A";
        } else {
            ss << snapuserd_client_->GetMergePercent() << "%";
        }
        ss << std::endl;
        ss << "Merge phase: " << update_status.merge_phase() << std::endl;
    }

    bool ok = true;
    std::vector<std::string> snapshots;
    if (!ListSnapshots(file.get(), &snapshots)) {
        LOG(ERROR) << "Could not list snapshots";
        snapshots.clear();
        ok = false;
    }
    for (const auto& name : snapshots) {
        ss << "Snapshot: " << name << std::endl;
        SnapshotStatus status;
        if (!ReadSnapshotStatus(file.get(), name, &status)) {
            ok = false;
            continue;
        }
        ss << "    state: " << SnapshotState_Name(status.state()) << std::endl;
        ss << "    device size (bytes): " << status.device_size() << std::endl;
        ss << "    snapshot size (bytes): " << status.snapshot_size() << std::endl;
        ss << "    cow partition size (bytes): " << status.cow_partition_size() << std::endl;
        ss << "    cow file size (bytes): " << status.cow_file_size() << std::endl;
        ss << "    allocated sectors: " << status.sectors_allocated() << std::endl;
        ss << "    metadata sectors: " << status.metadata_sectors() << std::endl;
        ss << "    compression: " << status.compression_algorithm() << std::endl;
        ss << "    compression factor: " << status.compression_factor() << std::endl;
        ss << "    merge phase: " << DecideMergePhase(status) << std::endl;
    }
    os << ss.rdbuf();
    return ok;
}

std::unique_ptr<AutoDevice> SnapshotManager::EnsureMetadataMounted() {
    if (!device_->IsRecovery()) {
        // No need to mount anything in recovery.
        LOG(INFO) << "EnsureMetadataMounted does nothing in Android mode.";
        return std::unique_ptr<AutoUnmountDevice>(new AutoUnmountDevice());
    }
    auto ret = AutoUnmountDevice::New(device_->GetMetadataDir());
    if (ret == nullptr) return nullptr;

    // In rescue mode, it is possible to erase and format metadata, but /metadata/ota is not
    // created to execute snapshot updates. Hence, subsequent calls is likely to fail because
    // Lock*() fails. By failing early and returning nullptr here, update_engine_sideload can
    // treat this case as if /metadata is not mounted.
    if (!LockShared()) {
        LOG(WARNING) << "/metadata is mounted, but errors occur when acquiring a shared lock. "
                        "Subsequent calls to SnapshotManager will fail. Unmounting /metadata now.";
        return nullptr;
    }
    return ret;
}

bool SnapshotManager::HandleImminentDataWipe(const std::function<void()>& callback) {
    if (!device_->IsRecovery()) {
        LOG(ERROR) << "Data wipes are only allowed in recovery.";
        return false;
    }

    auto mount = EnsureMetadataMounted();
    if (!mount || !mount->HasDevice()) {
        // We allow the wipe to continue, because if we can't mount /metadata,
        // it is unlikely the device would have booted anyway. If there is no
        // metadata partition, then the device predates Virtual A/B.
        LOG(INFO) << "/metadata not found; allowing wipe.";
        return true;
    }

    // This could happen if /metadata mounted but there is no filesystem
    // structure. Weird, but we have to assume there's no OTA pending, and
    // thus we let the wipe proceed.
    UpdateState state;
    {
        auto lock = LockExclusive();
        if (!lock) {
            LOG(ERROR) << "Unable to determine update state; allowing wipe.";
            return true;
        }

        state = ReadUpdateState(lock.get());
        LOG(INFO) << "Update state before wipe: " << state << "; slot: " << GetCurrentSlot()
                  << "; suffix: " << device_->GetSlotSuffix();
    }

    bool try_merge = false;
    switch (state) {
        case UpdateState::None:
        case UpdateState::Initiated:
            LOG(INFO) << "Wipe is not impacted by update state; allowing wipe.";
            break;
        case UpdateState::Unverified:
            if (GetCurrentSlot() != Slot::Target) {
                LOG(INFO) << "Wipe is not impacted by rolled back update; allowing wipe";
                break;
            }
            if (!HasForwardMergeIndicator()) {
                auto slot_number = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
                auto other_slot_number = SlotNumberForSlotSuffix(device_->GetOtherSlotSuffix());

                // We're not allowed to forward merge, so forcefully rollback the
                // slot switch.
                LOG(INFO) << "Allowing wipe due to lack of forward merge indicator; reverting to "
                             "old slot since update will be deleted.";
                device_->SetSlotAsUnbootable(slot_number);
                device_->SetActiveBootSlot(other_slot_number);
                break;
            }

            // Forward merge indicator means we have to mount snapshots and try to merge.
            LOG(INFO) << "Forward merge indicator is present.";
            try_merge = true;
            break;
        case UpdateState::Merging:
        case UpdateState::MergeFailed:
            try_merge = true;
            break;
        case UpdateState::MergeNeedsReboot:
        case UpdateState::Cancelled:
            LOG(INFO) << "Unexpected update state in recovery; allowing wipe.";
            break;
        default:
            break;
    }

    if (try_merge) {
        auto slot_number = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
        auto super_path = device_->GetSuperDevice(slot_number);
        if (!CreateLogicalAndSnapshotPartitions(super_path, 20s)) {
            LOG(ERROR) << "Unable to map partitions to complete merge.";
            return false;
        }

        auto process_callback = [&]() -> bool {
            if (callback) {
                callback();
            }
            return true;
        };

        state = ProcessUpdateStateOnDataWipe(process_callback);
        if (state == UpdateState::MergeFailed) {
            return false;
        }

        // Nothing should be depending on partitions now, so unmap them all.
        if (!UnmapAllPartitionsInRecovery()) {
            LOG(ERROR) << "Unable to unmap all partitions; fastboot may fail to flash.";
        }
    }

    if (state != UpdateState::None) {
        auto lock = LockExclusive();
        if (!lock) return false;

        // Zap the update state so the bootloader doesn't think we're still
        // merging. It's okay if this fails, it's informative only at this
        // point.
        WriteUpdateState(lock.get(), UpdateState::None);
    }
    return true;
}

bool SnapshotManager::FinishMergeInRecovery() {
    if (!device_->IsRecovery()) {
        LOG(ERROR) << "Data wipes are only allowed in recovery.";
        return false;
    }

    auto mount = EnsureMetadataMounted();
    if (!mount || !mount->HasDevice()) {
        return false;
    }

    auto slot_number = SlotNumberForSlotSuffix(device_->GetSlotSuffix());
    auto super_path = device_->GetSuperDevice(slot_number);
    if (!CreateLogicalAndSnapshotPartitions(super_path, 20s)) {
        LOG(ERROR) << "Unable to map partitions to complete merge.";
        return false;
    }

    UpdateState state = ProcessUpdateState();
    if (state != UpdateState::MergeCompleted) {
        LOG(ERROR) << "Merge returned unexpected status: " << state;
        return false;
    }

    // Nothing should be depending on partitions now, so unmap them all.
    if (!UnmapAllPartitionsInRecovery()) {
        LOG(ERROR) << "Unable to unmap all partitions; fastboot may fail to flash.";
    }
    return true;
}

UpdateState SnapshotManager::ProcessUpdateStateOnDataWipe(const std::function<bool()>& callback) {
    while (true) {
        UpdateState state = ProcessUpdateState(callback);
        LOG(INFO) << "Processed updated state in recovery: " << state;
        switch (state) {
            case UpdateState::MergeFailed:
                LOG(ERROR) << "Unrecoverable merge failure detected.";
                return state;
            case UpdateState::Unverified: {
                // Unverified was already handled earlier, in HandleImminentDataWipe,
                // but it will fall through here if a forward merge is required.
                //
                // If InitiateMerge fails, we early return. If it succeeds, then we
                // are guaranteed that the next call to ProcessUpdateState will not
                // return Unverified.
                if (!InitiateMerge()) {
                    LOG(ERROR) << "Failed to initiate merge on data wipe.";
                    return UpdateState::MergeFailed;
                }
                continue;
            }
            case UpdateState::MergeNeedsReboot:
                // We shouldn't get here, because nothing is depending on
                // logical partitions.
                LOG(ERROR) << "Unexpected merge-needs-reboot state in recovery.";
                return state;
            default:
                return state;
        }
    }
}

bool SnapshotManager::HasForwardMergeIndicator() {
    return access(GetForwardMergeIndicatorPath().c_str(), F_OK) == 0;
}

bool SnapshotManager::EnsureNoOverflowSnapshot(LockedFile* lock) {
    CHECK(lock);

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock, &snapshots)) {
        LOG(ERROR) << "Could not list snapshots.";
        return false;
    }

    for (const auto& snapshot : snapshots) {
        SnapshotStatus status;
        if (!ReadSnapshotStatus(lock, snapshot, &status)) {
            return false;
        }
        if (status.using_snapuserd()) {
            continue;
        }

        std::vector<DeviceMapper::TargetInfo> targets;
        if (!dm_.GetTableStatus(snapshot, &targets)) {
            LOG(ERROR) << "Could not read snapshot device table: " << snapshot;
            return false;
        }
        if (targets.size() != 1) {
            LOG(ERROR) << "Unexpected device-mapper table for snapshot: " << snapshot
                       << ", size = " << targets.size();
            return false;
        }
        if (targets[0].IsOverflowSnapshot()) {
            LOG(ERROR) << "Detected overflow in snapshot " << snapshot
                       << ", CoW device size computation is wrong!";
            return false;
        }
    }

    return true;
}

CreateResult SnapshotManager::RecoveryCreateSnapshotDevices() {
    if (!device_->IsRecovery()) {
        LOG(ERROR) << __func__ << " is only allowed in recovery.";
        return CreateResult::NOT_CREATED;
    }

    auto mount = EnsureMetadataMounted();
    if (!mount || !mount->HasDevice()) {
        LOG(ERROR) << "Couldn't mount Metadata.";
        return CreateResult::NOT_CREATED;
    }
    return RecoveryCreateSnapshotDevices(mount);
}

CreateResult SnapshotManager::RecoveryCreateSnapshotDevices(
        const std::unique_ptr<AutoDevice>& metadata_device) {
    if (!device_->IsRecovery()) {
        LOG(ERROR) << __func__ << " is only allowed in recovery.";
        return CreateResult::NOT_CREATED;
    }

    if (metadata_device == nullptr || !metadata_device->HasDevice()) {
        LOG(ERROR) << "Metadata not mounted.";
        return CreateResult::NOT_CREATED;
    }

    auto state_file = GetStateFilePath();
    if (access(state_file.c_str(), F_OK) != 0 && errno == ENOENT) {
        LOG(ERROR) << "Couldn't access state file.";
        return CreateResult::NOT_CREATED;
    }

    if (!NeedSnapshotsInFirstStageMount()) {
        return CreateResult::NOT_CREATED;
    }

    auto slot_suffix = device_->GetOtherSlotSuffix();
    auto slot_number = SlotNumberForSlotSuffix(slot_suffix);
    auto super_path = device_->GetSuperDevice(slot_number);
    if (!CreateLogicalAndSnapshotPartitions(super_path, 20s)) {
        LOG(ERROR) << "Unable to map partitions.";
        return CreateResult::ERROR;
    }
    return CreateResult::CREATED;
}

bool SnapshotManager::UpdateForwardMergeIndicator(bool wipe) {
    auto path = GetForwardMergeIndicatorPath();

    if (!wipe) {
        LOG(INFO) << "Wipe is not scheduled. Deleting forward merge indicator.";
        return RemoveFileIfExists(path);
    }

    // TODO(b/152094219): Don't forward merge if no CoW file is allocated.

    LOG(INFO) << "Wipe will be scheduled. Allowing forward merge of snapshots.";
    if (!android::base::WriteStringToFile("1", path)) {
        PLOG(ERROR) << "Unable to write forward merge indicator: " << path;
        return false;
    }

    return true;
}

ISnapshotMergeStats* SnapshotManager::GetSnapshotMergeStatsInstance() {
    return SnapshotMergeStats::GetInstance(*this);
}

// This is only to be used in recovery or normal Android (not first-stage init).
// We don't guarantee dm paths are available in first-stage init, because ueventd
// isn't running yet.
bool SnapshotManager::GetMappedImageDevicePath(const std::string& device_name,
                                               std::string* device_path) {
    // Try getting the device string if it is a device mapper device.
    if (dm_.GetState(device_name) != DmDeviceState::INVALID) {
        return dm_.GetDmDevicePathByName(device_name, device_path);
    }

    // Otherwise, get path from IImageManager.
    return images_->GetMappedImageDevice(device_name, device_path);
}

bool SnapshotManager::GetMappedImageDeviceStringOrPath(const std::string& device_name,
                                                       std::string* device_string_or_mapped_path) {
    // Try getting the device string if it is a device mapper device.
    if (dm_.GetState(device_name) != DmDeviceState::INVALID) {
        return dm_.GetDeviceString(device_name, device_string_or_mapped_path);
    }

    // Otherwise, get path from IImageManager.
    if (!images_->GetMappedImageDevice(device_name, device_string_or_mapped_path)) {
        return false;
    }

    LOG(WARNING) << "Calling GetMappedImageDevice with local image manager; device "
                 << (device_string_or_mapped_path ? *device_string_or_mapped_path : "(nullptr)")
                 << "may not be available in first stage init! ";
    return true;
}

bool SnapshotManager::WaitForDevice(const std::string& device,
                                    std::chrono::milliseconds timeout_ms) {
    if (!android::base::StartsWith(device, "/")) {
        return true;
    }

    // In first-stage init, we rely on init setting a callback which can
    // regenerate uevents and populate /dev for us.
    if (uevent_regen_callback_) {
        if (!uevent_regen_callback_(device)) {
            LOG(ERROR) << "Failed to find device after regenerating uevents: " << device;
            return false;
        }
        return true;
    }

    // Otherwise, the only kind of device we need to wait for is a dm-user
    // misc device. Normal calls to DeviceMapper::CreateDevice() guarantee
    // the path has been created.
    if (!android::base::StartsWith(device, "/dev/dm-user/")) {
        return true;
    }

    if (timeout_ms.count() == 0) {
        LOG(ERROR) << "No timeout was specified to wait for device: " << device;
        return false;
    }
    if (!android::fs_mgr::WaitForFile(device, timeout_ms)) {
        LOG(ERROR) << "Timed out waiting for device to appear: " << device;
        return false;
    }
    return true;
}

bool SnapshotManager::IsSnapuserdRequired() {
    auto lock = LockExclusive();
    if (!lock) return false;

    auto status = ReadSnapshotUpdateStatus(lock.get());
    return status.state() != UpdateState::None && status.using_snapuserd();
}

bool SnapshotManager::PrepareSnapuserdArgsForSelinux(std::vector<std::string>* snapuserd_argv) {
    return PerformInitTransition(InitTransition::SELINUX_DETACH, snapuserd_argv);
}

bool SnapshotManager::DetachFirstStageSnapuserdForSelinux() {
    LOG(INFO) << "Detaching first stage snapuserd";

    auto lock = LockExclusive();
    if (!lock) return false;

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots)) {
        LOG(ERROR) << "Failed to list snapshots.";
        return false;
    }

    size_t num_cows = 0;
    size_t ok_cows = 0;
    for (const auto& snapshot : snapshots) {
        std::string user_cow_name = GetDmUserCowName(snapshot, GetSnapshotDriver(lock.get()));

        if (dm_.GetState(user_cow_name) == DmDeviceState::INVALID) {
            continue;
        }

        DeviceMapper::TargetInfo target;
        if (!GetSingleTarget(user_cow_name, TableQuery::Table, &target)) {
            continue;
        }

        auto target_type = DeviceMapper::GetTargetType(target.spec);
        if (target_type != "user") {
            LOG(ERROR) << "Unexpected target type for " << user_cow_name << ": " << target_type;
            continue;
        }

        num_cows++;
        auto misc_name = user_cow_name;

        DmTable table;
        table.Emplace<DmTargetUser>(0, target.spec.length, misc_name);
        if (!dm_.LoadTableAndActivate(user_cow_name, table)) {
            LOG(ERROR) << "Unable to swap tables for " << misc_name;
            continue;
        }

        // Wait for ueventd to acknowledge and create the control device node.
        std::string control_device = "/dev/dm-user/" + misc_name;
        if (!WaitForDevice(control_device, 10s)) {
            LOG(ERROR) << "dm-user control device no found:  " << misc_name;
            continue;
        }

        ok_cows++;
        LOG(INFO) << "control device is ready: " << control_device;
    }

    if (ok_cows != num_cows) {
        LOG(ERROR) << "Could not transition all snapuserd consumers.";
        return false;
    }

    return true;
}

bool SnapshotManager::PerformSecondStageInitTransition() {
    return PerformInitTransition(InitTransition::SECOND_STAGE);
}

const LpMetadata* SnapshotManager::ReadOldPartitionMetadata(LockedFile* lock) {
    CHECK(lock);

    if (!old_partition_metadata_) {
        auto path = GetOldPartitionMetadataPath();
        old_partition_metadata_ = android::fs_mgr::ReadFromImageFile(path);
        if (!old_partition_metadata_) {
            LOG(ERROR) << "Could not read old partition metadata from " << path;
            return nullptr;
        }
    }
    return old_partition_metadata_.get();
}

MergePhase SnapshotManager::DecideMergePhase(const SnapshotStatus& status) {
    if (status.using_snapuserd() && status.device_size() < status.old_partition_size()) {
        return MergePhase::FIRST_PHASE;
    }
    return MergePhase::SECOND_PHASE;
}

void SnapshotManager::UpdateCowStats(ISnapshotMergeStats* stats) {
    auto lock = LockExclusive();
    if (!lock) return;

    std::vector<std::string> snapshots;
    if (!ListSnapshots(lock.get(), &snapshots, GetSnapshotSlotSuffix())) {
        LOG(ERROR) << "Could not list snapshots";
        return;
    }

    uint64_t cow_file_size = 0;
    uint64_t total_cow_size = 0;
    uint64_t estimated_cow_size = 0;
    for (const auto& snapshot : snapshots) {
        SnapshotStatus status;
        if (!ReadSnapshotStatus(lock.get(), snapshot, &status)) {
            return;
        }

        cow_file_size += status.cow_file_size();
        total_cow_size += status.cow_file_size() + status.cow_partition_size();
        estimated_cow_size += status.estimated_cow_size();
    }

    stats->report()->set_cow_file_size(cow_file_size);
    stats->report()->set_total_cow_size_bytes(total_cow_size);
    stats->report()->set_estimated_cow_size_bytes(estimated_cow_size);
}

void SnapshotManager::SetMergeStatsFeatures(ISnapshotMergeStats* stats) {
    auto lock = LockExclusive();
    if (!lock) return;

    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock.get());
    stats->report()->set_iouring_used(update_status.io_uring_enabled());
    stats->report()->set_userspace_snapshots_used(update_status.userspace_snapshots());
    stats->report()->set_xor_compression_used(GetXorCompressionEnabledProperty());
}

bool SnapshotManager::DeleteDeviceIfExists(const std::string& name,
                                           const std::chrono::milliseconds& timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (dm_.DeleteDeviceIfExists(name)) {
            return true;
        }
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        if (elapsed >= timeout_ms) {
            break;
        }
        std::this_thread::sleep_for(400ms);
    }

    // Try to diagnose why this failed. First get the actual device path.
    std::string full_path;
    if (!dm_.GetDmDevicePathByName(name, &full_path)) {
        LOG(ERROR) << "Unable to diagnose DM_DEV_REMOVE failure.";
        return false;
    }

    // Check for child dm-devices.
    std::string block_name = android::base::Basename(full_path);
    std::string sysfs_holders = "/sys/class/block/" + block_name + "/holders";

    std::error_code ec;
    std::filesystem::directory_iterator dir_iter(sysfs_holders, ec);
    if (auto begin = std::filesystem::begin(dir_iter); begin != std::filesystem::end(dir_iter)) {
        LOG(ERROR) << "Child device-mapper device still mapped: " << begin->path();
        return false;
    }

    // Check for mounted partitions.
    android::fs_mgr::Fstab fstab;
    android::fs_mgr::ReadFstabFromFile("/proc/mounts", &fstab);
    for (const auto& entry : fstab) {
        if (android::base::Basename(entry.blk_device) == block_name) {
            LOG(ERROR) << "Partition still mounted: " << entry.mount_point;
            return false;
        }
    }

    // Check for detached mounted partitions.
    for (const auto& fs : std::filesystem::directory_iterator("/sys/fs", ec)) {
        std::string fs_type = android::base::Basename(fs.path().c_str());
        if (!(fs_type == "ext4" || fs_type == "f2fs")) {
            continue;
        }

        std::string path = fs.path().c_str() + "/"s + block_name;
        if (access(path.c_str(), F_OK) == 0) {
            LOG(ERROR) << "Block device was lazily unmounted and is still in-use: " << full_path
                       << "; possibly open file descriptor or attached loop device.";
            return false;
        }
    }

    LOG(ERROR) << "Device-mapper device " << name << "(" << full_path << ")" << " still in use."
               << "  Probably a file descriptor was leaked or held open, or a loop device is"
               << " attached.";
    return false;
}

MergeFailureCode SnapshotManager::ReadMergeFailureCode() {
    auto lock = LockExclusive();
    if (!lock) return MergeFailureCode::AcquireLock;

    SnapshotUpdateStatus status = ReadSnapshotUpdateStatus(lock.get());
    if (status.state() != UpdateState::MergeFailed) {
        return MergeFailureCode::Ok;
    }
    return status.merge_failure_code();
}

std::string SnapshotManager::ReadSourceBuildFingerprint() {
    auto lock = LockExclusive();
    if (!lock) return {};

    SnapshotUpdateStatus status = ReadSnapshotUpdateStatus(lock.get());
    return status.source_build_fingerprint();
}

bool SnapshotManager::PauseSnapshotMerge() {
    auto snapuserd_client = SnapuserdClient::TryConnect(kSnapuserdSocket, 5s);
    if (snapuserd_client) {
        // Pause the snapshot-merge
        return snapuserd_client->PauseMerge();
    }
    return false;
}

bool SnapshotManager::ResumeSnapshotMerge() {
    auto snapuserd_client = SnapuserdClient::TryConnect(kSnapuserdSocket, 5s);
    if (snapuserd_client) {
        // Resume the snapshot-merge
        return snapuserd_client->ResumeMerge();
    }
    return false;
}

bool SnapshotManager::IsUserspaceSnapshotUpdateInProgress(
        std::vector<std::string>& dynamic_partitions) {
    // We cannot grab /metadata/ota lock here as this
    // is in reboot path. See b/308900853
    //
    // Check if any of the partitions are mounted
    // off dm-user block device. If so, then we are certain
    // that OTA update in progress.
    auto current_suffix = device_->GetSlotSuffix();
    auto& dm = DeviceMapper::Instance();
    auto dm_block_devices = dm.FindDmPartitions();
    if (dm_block_devices.empty()) {
        LOG(ERROR) << "No dm-enabled block device is found.";
        return false;
    }

    bool is_ota_in_progress = false;
    for (auto& partition : dm_block_devices) {
        std::string partition_name = partition.first + current_suffix;
        DeviceMapper::TargetInfo snap_target;
        if (!GetSingleTarget(partition_name, TableQuery::Status, &snap_target)) {
            continue;
        }
        auto type = DeviceMapper::GetTargetType(snap_target.spec);
        // Partition is mounted off snapshots
        if (type == "user") {
            dynamic_partitions.emplace_back("/" + partition.first);
            is_ota_in_progress = true;
        }
    }
    return is_ota_in_progress;
}

bool SnapshotManager::BootFromSnapshotsWithoutSlotSwitch() {
    auto lock = LockExclusive();
    if (!lock) return false;

    auto contents = device_->GetSlotSuffix();
    // This is the indicator which tells first-stage init
    // to boot from snapshots even though there was no slot-switch
    auto boot_file = GetBootSnapshotsWithoutSlotSwitchPath();
    if (!WriteStringToFileAtomic(contents, boot_file)) {
        PLOG(ERROR) << "write failed: " << boot_file;
        return false;
    }

    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock.get());
    update_status.set_state(UpdateState::Initiated);
    update_status.set_userspace_snapshots(true);
    update_status.set_using_snapuserd(true);
    if (!WriteSnapshotUpdateStatus(lock.get(), update_status)) {
        return false;
    }
    return true;
}

bool SnapshotManager::PrepareDeviceToBootWithoutSnapshot() {
    auto lock = LockExclusive();
    if (!lock) return false;

    android::base::RemoveFileIfExists(GetSnapshotBootIndicatorPath());
    android::base::RemoveFileIfExists(GetBootSnapshotsWithoutSlotSwitchPath());

    SnapshotUpdateStatus update_status = ReadSnapshotUpdateStatus(lock.get());
    update_status.set_state(UpdateState::Cancelled);
    if (!WriteSnapshotUpdateStatus(lock.get(), update_status)) {
        return false;
    }
    return true;
}

void SnapshotManager::SetReadAheadSize(const std::string& entry_block_device, off64_t size_kb) {
    std::string block_device;
    if (!Realpath(entry_block_device, &block_device)) {
        PLOG(ERROR) << "Failed to realpath " << entry_block_device;
        return;
    }

    static constexpr std::string_view kDevBlockPrefix("/dev/block/");
    if (!android::base::StartsWith(block_device, kDevBlockPrefix)) {
        LOG(ERROR) << block_device << " is not a block device";
        return;
    }

    std::string block_name = block_device.substr(kDevBlockPrefix.length());
    std::string sys_partition =
            android::base::StringPrintf("/sys/class/block/%s/partition", block_name.c_str());
    struct stat info;
    if (lstat(sys_partition.c_str(), &info) == 0) {
        block_name += "/..";
    }
    std::string sys_ra = android::base::StringPrintf("/sys/class/block/%s/queue/read_ahead_kb",
                                                     block_name.c_str());
    std::string size = std::to_string(size_kb);
    android::base::WriteStringToFile(size, sys_ra.c_str());
}

}  // namespace snapshot
}  // namespace android
