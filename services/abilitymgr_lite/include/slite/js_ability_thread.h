/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
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

#ifndef OHOS_ABILITY_SLITE_JS_ABILITY_THREAD_H
#define OHOS_ABILITY_SLITE_JS_ABILITY_THREAD_H

#include <cstdint>
#include "ability_record.h"
#include "ability_thread.h"

namespace OHOS {
namespace AbilitySlite {
class JsAbilityThread : public AbilityThread {
public:
    JsAbilityThread();

    ~JsAbilityThread() override;

    int32_t InitAbilityThread(const AbilityRecord *abilityRecord) override;

    int32_t ReleaseAbilityThread() override;

    osMessageQueueId_t GetMessageQueueId() const override;

    osThreadId_t GetAppTaskId() const override;

    static void AppTaskHandler(void *argument);
private:
    int32_t CreateAppTask(bool &needUnlockKernel);

    static bool ProcessMessage(AbilityThread *abilityThread, AbilityThread *&defaultAbilityThread,
        SliteAbilityInnerMsg &innerMsg);

    osMessageQueueId_t messageQueueId_ = nullptr;
    osThreadId_t appTaskId_ = nullptr;
};
} // namespace AbilitySlite
} // namespace OHOS

#endif // OHOS_ABILITY_SLITE_JS_ABILITY_THREAD_H
