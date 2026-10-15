/*
 * Copyright (c) 2020 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstring>
#include "abilityms_log.h"
#include "ability_errors.h"
#include "ability_list.h"
#include "ability_lock_guard.h"
#include "ability_record.h"
#include "ability_record_observer_manager.h"

namespace OHOS {
namespace AbilitySlite {
AbilityList::AbilityList()
{
    abilityListMutex_ = osMutexNew(reinterpret_cast<osMutexAttr_t *>(NULL));
}

AbilityList::~AbilityList()
{
    osMutexDelete(abilityListMutex_);
}

void AbilityList::Add(AbilityRecord *abilityRecord)
{
    AbilityLockGuard locker(abilityListMutex_);
    if (abilityRecord == nullptr) {
        return;
    }
    if (abilityList_.Size() >= ABILITY_LIST_CAPACITY) {
        PopBottomAbility();
    }

    if (Get(abilityRecord->token) == nullptr) {
        abilityList_.PushFront(abilityRecord);
    }
}

AbilityRecord *AbilityList::Get(uint16_t token) const
{
    AbilityLockGuard locker(abilityListMutex_);
    for (auto node = abilityList_.Begin(); node != abilityList_.End(); node = node->next_) {
        AbilityRecord *record = node->value_;
        if (record == nullptr) {
            continue;
        }
        if (record->token == token) {
            return record;
        }
    }

    return nullptr;
}

AbilityRecord *AbilityList::Get(const char *bundleName) const
{
    if (bundleName == nullptr) {
        return nullptr;
    }

    AbilityLockGuard locker(abilityListMutex_);
    for (auto node = abilityList_.Begin(); node != abilityList_.End(); node = node->next_) {
        AbilityRecord *record = node->value_;
        if (record == nullptr || record->appName == nullptr) {
            continue;
        }
        if (strcmp(bundleName, record->appName) == 0) {
            return record;
        }
    }
    return nullptr;
}

AbilityRecord *AbilityList::GetByTaskId(osThreadId_t taskId) const
{
    AbilityLockGuard locker(abilityListMutex_);
    for (auto node = abilityList_.Begin(); node != abilityList_.End(); node = node->next_) {
        AbilityRecord *record = node->value_;
        if (record == nullptr) {
            continue;
        }
        if (record->taskId == taskId) {
            return record;
        }
    }
    return nullptr;
}

void AbilityList::Erase(uint16_t token)
{
    AbilityLockGuard locker(abilityListMutex_);
    for (auto node = abilityList_.Begin(); node != abilityList_.End(); node = node->next_) {
        AbilityRecord *record = node->value_;
        if (record == nullptr) {
            continue;
        }
        if (record->token == token) {
            abilityList_.Remove(node);
            return;
        }
    }
}

void AbilityList::GetAbilityList(uint32_t mission, List<uint32_t> &result)
{
    AbilityLockGuard locker(abilityListMutex_);

    for (auto node = abilityList_.Begin(); node != abilityList_.End(); node = node->next_) {
        AbilityRecord *record = node->value_;
        if ((record != nullptr) && (record->mission == mission)) {
            result.PushFront(record->token);
        }
    }
}

uint32_t AbilityList::Size() const
{
    AbilityLockGuard locker(abilityListMutex_);
    return abilityList_.Size();
}

bool AbilityList::MoveToTop(uint16_t token)
{
    AbilityLockGuard locker(abilityListMutex_);
    AbilityRecord *abilityRecord = Get(token);
    if (abilityRecord == nullptr) {
        return false;
    }
    Erase(token);
    Add(abilityRecord);
    return true;
}

void AbilityList::PopAbility()
{
    AbilityLockGuard locker(abilityListMutex_);
    abilityList_.PopFront();
}

AbilityRecord *AbilityList::GetTopAbility() const
{
    AbilityLockGuard locker(abilityListMutex_);
    if (abilityList_.Size() != 0) {
        return abilityList_.Front();
    }
    return nullptr;
}

MissionInfoList *AbilityList::GetMissionInfos(uint32_t maxNum) const
{
    AbilityLockGuard lock(abilityListMutex_);
    MissionInfoList *missionInfoList = new MissionInfoList;
    if (missionInfoList == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Failed to new MissionInfoList.");
        return nullptr;
    }
    missionInfoList->length = abilityList_.Size();
    if (maxNum != 0) {
        missionInfoList->length = (missionInfoList->length > maxNum) ? maxNum : missionInfoList->length;
    }
    missionInfoList->missionInfos = new MissionInfo[missionInfoList->length];
    if (missionInfoList->missionInfos == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Failed to new missionInfos.");
        delete missionInfoList;
        return nullptr;
    }
    uint32_t i = 0;
    for (auto it = abilityList_.Begin(); i < missionInfoList->length; it = it->next_) {
        missionInfoList->missionInfos[i++].SetAppName(it->value_->appName);
    }
    return missionInfoList;
}

void AbilityList::PopBottomAbility()
{
    AbilityLockGuard locker(abilityListMutex_);
    AbilityRecord *lastRecord = abilityList_.Back();
    if (lastRecord == nullptr) {
        abilityList_.PopBack();
        return;
    }
    if (!IsPermanentAbility(*lastRecord)) {
        abilityList_.PopBack();
        delete lastRecord;
        return;
    }
    // last record is home
    abilityList_.PopBack(); // pop home
    AbilityRecord *secondLastRecord = abilityList_.Back();
    abilityList_.PopBack(); // pop secondLastRecord
    delete secondLastRecord;
    abilityList_.PushBack(lastRecord); // push back home
}

int32_t AbilityList::PopAllAbility(const char *excludedBundleName)
{
    AbilityLockGuard locker(abilityListMutex_);
    OHOS::List<AbilityRecord *> reservedRecordList {};
    AbilityRecord *topRecord = abilityList_.Front();
    if (topRecord == nullptr) {
        return PARAM_NULL_ERROR;
    }
    abilityList_.PopFront();
    reservedRecordList.PushFront(topRecord);

    while (abilityList_.Size() > 0) {
        AbilityRecord *record = abilityList_.Front();
        abilityList_.PopFront();
        if (record == nullptr) {
            continue;
        }
        if (AbilityList::IsPermanentAbility(*record)) {
            reservedRecordList.PushFront(record);
            continue;
        }
        if (excludedBundleName != nullptr && strcmp(record->appName, excludedBundleName) == 0) {
            reservedRecordList.PushFront(record);
            continue;
        }
        AbilityRecordObserverManager::GetInstance().NotifyAbilityRecordCleanup(record->appName);
        delete record;
    }
    while (reservedRecordList.Size() > 0) {
        AbilityRecord *record = reservedRecordList.Front();
        reservedRecordList.PopFront();
        if (record == nullptr) {
            continue;
        }
        abilityList_.PushFront(record);
    }
    return ERR_OK;
}

bool AbilityList::IsPermanentAbility(const AbilityRecord &abilityRecord)
{
    if (abilityRecord.appName == nullptr) {
        return false;
    }
    if (abilityRecord.token == LAUNCHER_TOKEN) {
        return true;
    }
    if (strcmp(abilityRecord.appName, MAIN_BUNDLE_NAME) == 0) {
        return true;
    }
    return false;
}
} // namespace AbilitySlite
} // namespace OHOS
