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

#ifndef OHOS_ABILITY_SLITE_ABILITY_SERVICE_H
#define OHOS_ABILITY_SLITE_ABILITY_SERVICE_H

#include <cstdlib>

#include "ability_list.h"
#include "ability_record.h"
#include "ability_record_observer.h"
#include "adapter.h"
#include "bms_helper.h"
#include "js_ability_thread.h"
#include "mission_info.h"
#include "nocopyable.h"
#include "want.h"
#include "slite_ability.h"
#include "slite_ability_state.h"

namespace OHOS {
namespace AbilitySlite {
struct AbilityOperation {
    AbilityOperation() {}
    ~AbilityOperation() {}

    uint16_t msgId = 0;
    Want *want = nullptr;
    uint64_t token = 0;
};

class AbilityRecordManager : public NoCopyable {
public:
    typedef void (AbilityRecordManager::*LifecycleFunc)(uint16_t token);

    struct LifecycleFuncStr {
        int32_t state;
        LifecycleFunc func_ptr;
    };

    static AbilityRecordManager &GetInstance()
    {
        static AbilityRecordManager instance;
        return instance;
    }

    ~AbilityRecordManager() override;

    int32_t StartAbility(const Want *want);

    int32_t TerminateAbility(uint16_t token);

    int32_t TerminateMission(uint32_t mission);

    int32_t TerminateAll(const char *excludedBundleName);

    int32_t ForceStop(const Want *want);

    int32_t ForceStopBundle(uint16_t token);

    int32_t SchedulerLifecycleDone(uint64_t token, int32_t state);

    ElementName *GetTopAbility();

    MissionInfoList *GetMissionInfos(uint32_t maxNum) const;

    void setNativeAbility(const SliteAbility *ability);

    void StartLauncher();

    void SetIsAppScheduling(bool runState);

    bool GetIsAppScheduling();

    int32_t RunOperation();

    int32_t AddAbilityRecordObserver(AbilityRecordObserver *observer);
    int32_t RemoveAbilityRecordObserver(AbilityRecordObserver *observer);

    osThreadId_t curTask_ = nullptr;

private:
    static uint16_t GenerateToken();

    uint32_t GenerateMission();

    AbilityRecordManager();

    int32_t StartAbility(AbilitySvcInfo *info);

    int32_t StartAbility(const AbilityRecord *record);

    int32_t StartRemoteAbility(const Want *want);

    int32_t PreCheckStartAbility(const AbilitySvcInfo &info);

    bool CheckResponse(const char *bundleName);

    int32_t SchedulerLifecycle(uint64_t token, int32_t state);

    int32_t ScheduleLifecycleInner(const AbilityRecord *record, int32_t state);

    void SchedulerAbilityLifecycle(SliteAbility *ability, const Want &want, int32_t state);

    int32_t CreateAppTask(AbilityRecord *record);

    void OnCreateDone(uint16_t token);

    void OnForegroundDone(uint16_t token);

    void OnBackgroundDone(uint16_t token);

    void OnDestroyDone(uint16_t token);

    void DeleteRecordInfo(uint16_t token);

    void DeleteAbilityThread(AbilityRecord *record);

    int32_t SendMsgToAbilityThread(int32_t state, const AbilityRecord *record);

    void SetAbilityStateAndNotify(uint64_t token, int32_t state);

    void UpdateRecord(AbilitySvcInfo *info);

    int32_t TerminateAbility(uint16_t token, const Want* want);

    int32_t ForceStopBundleInner(uint16_t token);

    bool IsLauncher(const char *bundleName);

    Want *CreateWant(const AbilityRecord *record);

    bool NeedToBeTerminated(const char *bundleName);

    Want *CopyWant(const Want *want);

    int32_t AddAbilityOperation(uint16_t msgId, const Want *want, uint64_t token);

    uint16_t pendingToken_ { 0 };
#ifndef _MINI_MULTI_TASKS_
    AbilityRecord *pendingRecord = nullptr;
#endif
    List<AbilityOperation *> abilityOperation_ {};
    bool isAppScheduling_ = false;

    AbilityList abilityList_ {};
    SliteAbility *nativeAbility_ = nullptr;
};
} // namespace AbilitySlite
} // namespace OHOS
#endif  // OHOS_ABILITY_SLITE_ABILITY_SERVICE_H
