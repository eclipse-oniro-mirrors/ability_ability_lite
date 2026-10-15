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

#ifndef OHOS_ABILITY_SLITE_ABILITY_RECORD_H
#define OHOS_ABILITY_SLITE_ABILITY_RECORD_H

#include "ability_info.h"
#include "ability_saved_data.h"
#include "cmsis_os2.h"
#include "ability_thread.h"
#include "ability_record_state.h"

namespace OHOS {
namespace AbilitySlite {
struct AbilityData {
    AbilityData();

    ~AbilityData();

    void *wantData = nullptr;
    uint16_t wantDataSize = 0;
};

struct AbilityRecord {
public:
    AbilityRecord();

    ~AbilityRecord();

    void SetAppName(const char *name);

    void SetAppPath(const char *path);

    void SetWantData(const void *wantData, uint16_t wantDataSize);

    char *appName = nullptr;
    char *appPath = nullptr;
    AbilityData *abilityData = nullptr;
    AbilitySavedData *abilitySavedData = nullptr;
    AbilityThread *abilityThread = nullptr;
    osMessageQueueId_t jsAppQueueId = nullptr;
    osThreadId_t taskId = nullptr;
    uint16_t token = 0;
    uint32_t mission = 0;
    uint8_t state = SCHEDULE_STOP;
    bool isTerminated = false;
    bool isNativeApp = false;
};
} // namespace AbilitySlite
} // namespace OHOS
#endif // OHOS_ABILITY_SLITE_ABILITY_RECORD_H
