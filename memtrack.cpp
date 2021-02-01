/*
 * Copyright (C) 2013 The Android Open Source Project
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
#define LOG_TAG "memtrack"

#include <aidl/android/hardware/memtrack/IMemtrack.h>
#include <aidl/android/hardware/memtrack/MemtrackType.h>
#include <android/binder_manager.h>
#include <android/hardware/memtrack/1.0/IMemtrack.h>
#include <memtrack/memtrack.h>

#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <mutex>
#include <vector>

#include <log/log.h>

using android::hardware::memtrack::V1_0::IMemtrack;
using android::hardware::memtrack::V1_0::MemtrackType;
using android::hardware::memtrack::V1_0::MemtrackRecord;
using android::hardware::memtrack::V1_0::MemtrackFlag;
using android::hardware::memtrack::V1_0::MemtrackStatus;
using android::hardware::hidl_vec;
using android::hardware::Return;

namespace V1_0 = android::hardware::memtrack::V1_0;
namespace V_aidl = aidl::android::hardware::memtrack;

// Check Memtrack Flags
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::SMAPS_ACCOUNTED) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_SMAPS_ACCOUNTED));
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::SMAPS_UNACCOUNTED) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_SMAPS_UNACCOUNTED));
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::SHARED) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_SHARED));
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::SHARED_PSS) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_SHARED_PSS));
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::PRIVATE) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_PRIVATE));
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::SYSTEM) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_SYSTEM));
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::DEDICATED) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_DEDICATED));
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::NONSECURE) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_NONSECURE));
static_assert(static_cast<uint32_t>(V1_0::MemtrackFlag::SECURE) ==
              static_cast<uint32_t>(V_aidl::MemtrackRecord::FLAG_SECURE));

// Check Memtrack Types
static_assert(static_cast<uint32_t>(V1_0::MemtrackType::OTHER) ==
              static_cast<uint32_t>(V_aidl::MemtrackType::OTHER));
static_assert(static_cast<uint32_t>(V1_0::MemtrackType::GL) ==
              static_cast<uint32_t>(V_aidl::MemtrackType::GL));
static_assert(static_cast<uint32_t>(V1_0::MemtrackType::GRAPHICS) ==
              static_cast<uint32_t>(V_aidl::MemtrackType::GRAPHICS));
static_assert(static_cast<uint32_t>(V1_0::MemtrackType::MULTIMEDIA) ==
              static_cast<uint32_t>(V_aidl::MemtrackType::MULTIMEDIA));
static_assert(static_cast<uint32_t>(V1_0::MemtrackType::CAMERA) ==
              static_cast<uint32_t>(V_aidl::MemtrackType::CAMERA));

struct memtrack_proc_type {
    MemtrackType type;
    std::vector<MemtrackRecord> records;
};

struct memtrack_proc {
    pid_t pid;
    memtrack_proc_type types[static_cast<int>(MemtrackType::NUM_TYPES)];
};

//TODO(b/31632518)
static android::sp<V1_0::IMemtrack> get_hidl_instance() {
    static android::sp<IMemtrack> module = IMemtrack::getService();
    static bool logged = false;
    if (module == nullptr && !logged) {
        logged = true;
        ALOGE("Couldn't load memtrack module");
    }
    return module;
}

static std::shared_ptr<V_aidl::IMemtrack> get_aidl_instance() {
    const auto instance = std::string() + V_aidl::IMemtrack::descriptor + "/default";
    static bool declared = AServiceManager_isDeclared(instance.c_str());
    if (!declared) {
        return nullptr;
    }
    static ndk::SpAIBinder memtrack_binder =
            ndk::SpAIBinder(AServiceManager_waitForService(instance.c_str()));
    return V_aidl::IMemtrack::fromBinder(memtrack_binder);
}

bool memtrack_gpu_device_info(std::vector<V_aidl::DeviceInfo>* device_info) {
    std::shared_ptr<V_aidl::IMemtrack> service = get_aidl_instance();
    if (!service) {
        return false;
    }

    auto status = service->getGpuDeviceInfo(device_info);
    return status.isOk();
}

memtrack_proc *memtrack_proc_new(void)
{
    return new memtrack_proc();
}

void memtrack_proc_destroy(memtrack_proc *p)
{
    delete(p);
}

static int memtrack_proc_get_type(memtrack_proc_type *t,
        pid_t pid, MemtrackType type)
{
    int err = 0;

    std::shared_ptr<V_aidl::IMemtrack> service= get_aidl_instance();
    if (service) {
        std::vector<V_aidl::MemtrackRecord> records;
        auto status = service->getMemory(
                pid, static_cast<V_aidl::MemtrackType>(static_cast<uint32_t>(type)), &records);

        if (!status.isOk()) {
            return -1;
        }

        t->records.resize(records.size());
        for (size_t i = 0; i < records.size(); i++) {
            t->records[i].sizeInBytes = records[i].sizeInBytes;
            t->records[i].flags = records[i].flags;
        }

        return err;
    }

    android::sp<V1_0::IMemtrack> memtrack = get_hidl_instance();
    if (memtrack == nullptr)
        return -1;

    Return<void> ret = memtrack->getMemory(pid, type,
        [&t, &err](MemtrackStatus status, hidl_vec<MemtrackRecord> records) {
            if (status != MemtrackStatus::SUCCESS) {
                err = -1;
                t->records.resize(0);
            }
            t->records.resize(records.size());
            for (size_t i = 0; i < records.size(); i++) {
                t->records[i].sizeInBytes = records[i].sizeInBytes;
                t->records[i].flags = records[i].flags;
            }
    });
    return ret.isOk() ? err : -1;
}

/* TODO: sanity checks on return values from HALs:
 *   make sure no records have invalid flags set
 *    - unknown flags
 *    - too many flags of a single category
 *    - missing ACCOUNTED/UNACCOUNTED
 *   make sure there are not overlapping SHARED and SHARED_PSS records
 */
static int memtrack_proc_sanity_check(memtrack_proc* /*p*/)
{
    return 0;
}

int memtrack_proc_get(memtrack_proc *p, pid_t pid)
{
    if (!p) {
        return -EINVAL;
    }

    p->pid = pid;
    for (uint32_t i = 0; i < (uint32_t)MemtrackType::NUM_TYPES; i++) {
        int ret = memtrack_proc_get_type(&p->types[i], pid, (MemtrackType)i);
        if (ret != 0)
           return ret;
    }

    return memtrack_proc_sanity_check(p);
}

static ssize_t memtrack_proc_sum(memtrack_proc *p,
        const std::vector<MemtrackType>& types, uint32_t flags)
{
    ssize_t sum = 0;

    for (size_t i = 0; i < types.size(); i++) {
        memtrack_proc_type type = p->types[static_cast<int>(types[i])];
        std::vector<MemtrackRecord> records = type.records;
        for (size_t j = 0; j < records.size(); j++) {
            if ((records[j].flags & flags) == flags) {
                sum += records[j].sizeInBytes;
            }
        }
    }

    return sum;
}

ssize_t memtrack_proc_graphics_total(memtrack_proc *p)
{
    std::vector<MemtrackType> types = {MemtrackType::GRAPHICS};
    return memtrack_proc_sum(p, types, 0);
}

ssize_t memtrack_proc_graphics_pss(memtrack_proc *p)
{
    std::vector<MemtrackType> types = { MemtrackType::GRAPHICS };
    return memtrack_proc_sum(p, types,
            (uint32_t)MemtrackFlag::SMAPS_UNACCOUNTED);
}

ssize_t memtrack_proc_gl_total(memtrack_proc *p)
{
    std::vector<MemtrackType> types = { MemtrackType::GL };
    return memtrack_proc_sum(p, types, 0);
}

ssize_t memtrack_proc_gl_pss(memtrack_proc *p)
{
    std::vector<MemtrackType> types = { MemtrackType::GL };
    return memtrack_proc_sum(p, types,
            (uint32_t)MemtrackFlag::SMAPS_UNACCOUNTED);
}

ssize_t memtrack_proc_other_total(memtrack_proc *p)
{
    std::vector<MemtrackType> types = { MemtrackType::MULTIMEDIA,
            MemtrackType::CAMERA, MemtrackType::OTHER };
    return memtrack_proc_sum(p, types, 0);
}

ssize_t memtrack_proc_other_pss(memtrack_proc *p)
{
    std::vector<MemtrackType> types = { MemtrackType::MULTIMEDIA,
            MemtrackType::CAMERA, MemtrackType::OTHER };
    return memtrack_proc_sum(p, types,
            (uint32_t)MemtrackFlag::SMAPS_UNACCOUNTED);
}
