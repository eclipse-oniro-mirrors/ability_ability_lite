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

#ifndef OHOS_ABILITY_SLITE_ABILITY_LIST_H
#define OHOS_ABILITY_SLITE_ABILITY_LIST_H

#include "ability_record.h"
#include "cmsis_os2.h"
#include "mission_info.h"
#include "utils_list.h"

namespace OHOS {
namespace AbilitySlite {
constexpr char MAIN_BUNDLE_NAME[] = "main";
const uint32_t LAUNCHER_TOKEN = 0;

class AbilityList {
public:
    AbilityList();

    ~AbilityList();

    void Add(AbilityRecord *abilityRecord);

    AbilityRecord *Get(uint16_t token) const;

    AbilityRecord *Get(const char *bundleName) const;

    AbilityRecord *GetByTaskId(osThreadId_t taskId) const;

    void Erase(uint16_t token);

    uint32_t Size() const;

    bool MoveToTop(uint16_t token);

    void PopAbility();

    AbilityRecord *GetTopAbility() const;

    MissionInfoList *GetMissionInfos(uint32_t maxNum) const;

    void GetAbilityList(uint32_t mission, List<uint32_t> &result);

    void PopBottomAbility();

    int32_t PopAllAbility(const char *excludedBundleName);

    static bool IsPermanentAbility(const AbilityRecord &abilityRecord);

private:
    List<AbilityRecord *> abilityList_ {};
    mutable osMutexId_t abilityListMutex_;
};
} // AbilitySlite
} // namespace OHOS
#endif // OHOS_ABILITY_SLITE_ABILITY_LIST_H
