/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
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

#include "abilityms_slite_client.h"

#include "ability_errors.h"
#include "abilityms_log.h"
#include "adapter.h"
#include "cmsis_os2.h"
#include "samgr_lite.h"
#include "securec.h"
#include "want.h"
#include "utils.h"

namespace OHOS {
namespace AbilitySlite {
bool AbilityMsClient::Initialize() const
{
    if (amsProxy_ != nullptr) {
        return true;
    }
    int retry = RETRY_TIMES;
    while (retry--) {
        IUnknown *iUnknown = SAMGR_GetInstance()->GetFeatureApi(AMS_SERVICE, AMS_SLITE_FEATURE);
        if (iUnknown == nullptr) {
            HILOG_ERROR(HILOG_MODULE_APP, "iUnknown is null");
            osDelay(ERROR_SLEEP_TIMES); // sleep 300ms
            continue;
        }

        (void)iUnknown->QueryInterface(iUnknown, DEFAULT_VERSION, (void **)&amsProxy_);
        if (amsProxy_ == nullptr) {
            HILOG_ERROR(HILOG_MODULE_APP, "ams proxy is null");
            osDelay(ERROR_SLEEP_TIMES); // sleep 300ms
            continue;
        }
        return true;
    }
    return false;
}

int32_t AbilityMsClient::SendRequestToAms(Request &request) const
{
    int32_t retry = RETRY_TIMES;
    while (retry--) {
        int32_t ret = SAMGR_SendRequest(identity_, &request, nullptr);
        if (ret == EC_SUCCESS) {
            return ERR_OK;
        }
        HILOG_WARN(HILOG_MODULE_APP, "SendRequestToAms SAMGR_SendRequest failed with %{public}d", ret);
        osDelay(ERROR_SLEEP_TIMES); // sleep 300ms
    }
    HILOG_ERROR(HILOG_MODULE_APP, "SendRequestToAms failed.");
    return IPC_REQUEST_ERROR;
}

int32_t AbilityMsClient::StartAbility(const Want *want) const
{
    if (want == nullptr || want->element == nullptr || !Initialize()) {
        return PARAM_CHECK_ERROR;
    }

    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }

    // The data and info will be freed in the service
    auto *data = static_cast<StartAbilityData *>(AdapterMalloc(sizeof(StartAbilityData)));
    if (data == nullptr) {
        return MEMORY_MALLOC_ERROR;
    }
    Want *info = static_cast<Want *>(AdapterMalloc(sizeof(Want)));
    if (info == nullptr) {
        return MEMORY_MALLOC_ERROR;
    }
    info->element = nullptr;
    info->data = nullptr;
    info->dataLength = 0;
    info->appPath = nullptr;
    info->mission = want->mission;
    info->actions = nullptr;
    info->entities = nullptr;
    SetWantElement(info, *(want->element));
    if (want->data != nullptr) {
        HILOG_INFO(HILOG_MODULE_APP, "start ability with input data");
        SetWantData(info, want->data, want->dataLength);
    } else {
        SetWantData(info, nullptr, 0);
        HILOG_INFO(HILOG_MODULE_APP, "start ability with no data");
    }
    data->want = info;
    data->curTask = osThreadGetId();
    Request request = {
        .msgId = START_ABILITY,
        .len = sizeof(StartAbilityData),
        .data = data,
        .msgValue = 0,
    };
    return SendRequestToAms(request);
}

int32_t AbilityMsClient::TerminateAbility(uint64_t token) const
{
    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    Request request = {
        .msgId = TERMINATE_ABILITY,
        .len = 0,
        .data = nullptr,
        .msgValue = static_cast<uint32_t>(token & TRANSACTION_MSG_TOKEN_MASK),
    };
    return SendRequestToAms(request);
}

int32_t AbilityMsClient::TerminateMission(uint32_t mission) const
{
    Request request = {
        .msgId = TERMINATE_MISSION,
        .len = 0,
        .data = nullptr,
        .msgValue = mission,
    };
    return SendRequestToAms(request);
}

int32_t AbilityMsClient::TerminateAll(const char* excludedBundleName) const
{
    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    void *data = nullptr;
    if (excludedBundleName != nullptr) {
        data = Utils::Strdup(excludedBundleName);
    }
    Request request = {
        .msgId = TERMINATE_ALL,
        .len = 0,
        .data = data,
        .msgValue = 0,
    };
    return SendRequestToAms(request);
}

int32_t AbilityMsClient::SchedulerLifecycleDone(uint64_t token, int32_t state) const
{
    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    Request request = {
        .msgId = ABILITY_TRANSACTION_DONE,
        .len = 0,
        .data = nullptr,
        .msgValue = static_cast<uint32_t>((token & TRANSACTION_MSG_TOKEN_MASK) |
                                          (state << TRANSACTION_MSG_STATE_OFFSET)),
    };
    return SendRequestToAms(request);
}

int32_t AbilityMsClient::ForceStopBundle(uint64_t token) const
{
    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    Request request = {
        .msgId = TERMINATE_APP,
        .len = 0,
        .data = nullptr,
        .msgValue = static_cast<uint32_t>(token & TRANSACTION_MSG_TOKEN_MASK),
    };
    return SendRequestToAms(request);
}

ElementName *AbilityMsClient::GetTopAbility() const
{
    if (!Initialize()) {
        return nullptr;
    }
    return amsProxy_->GetTopAbility();
}

int32_t AbilityMsClient::ForceStop(const char *bundleName) const
{
    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    Want *want = static_cast<Want *>(AdapterMalloc(sizeof(Want)));
    if (want == nullptr) {
        return MEMORY_MALLOC_ERROR;
    }
    want->element = reinterpret_cast<ElementName *>(AdapterMalloc(sizeof(ElementName)));
    if (want->element == nullptr) {
        return MEMORY_MALLOC_ERROR;
    }
    HILOG_INFO(HILOG_MODULE_APP, "ForceStop with bundleName");
    want->element->deviceId = nullptr;
    want->element->bundleName = Utils::Strdup(bundleName);
    want->element->abilityName = nullptr;
    want->data = nullptr;
    want->dataLength = 0;
    want->appPath = nullptr;
    want->actions = nullptr;
    want->entities = nullptr;
    SetWantData(want, nullptr, 0);
    Request request = {
        .msgId = TERMINATE_APP_BY_BUNDLENAME,
        .len = sizeof(Want),
        .data = want,
    };
    return SendRequestToAms(request);
}

int32_t AbilityMsClient::ForceStop(const Want *want) const
{
    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    if (want == nullptr || want->element == nullptr || want->element->bundleName == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    Want *info = static_cast<Want *>(AdapterMalloc(sizeof(Want)));
    if (info == nullptr) {
        return MEMORY_MALLOC_ERROR;
    }
    info->element = nullptr;
    info->data = nullptr;
    info->dataLength = 0;
    info->appPath = nullptr;
    SetWantElement(info, *(want->element));
    SetWantData(info, want->data, want->dataLength);
    if (want->data != nullptr) {
        HILOG_INFO(HILOG_MODULE_APP, "ForceStop with data");
    } else {
        HILOG_INFO(HILOG_MODULE_APP, "ForceStop with no data");
    }
    Request request = {
        .msgId = TERMINATE_APP_BY_BUNDLENAME,
        .len = sizeof(Want),
        .data = info,
    };
    return SendRequestToAms(request);
}

int32_t AbilityMsClient::AddAbilityRecordObserver(AbilityRecordObserver *observer)
{
    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    Request request = {
        .msgId = ADD_ABILITY_RECORD_OBSERVER,
        .len = 0,
        .data = nullptr,
        .msgValue = reinterpret_cast<uint32>(observer),
    };

    return SendRequestToAms(request);
}

int32_t AbilityMsClient::RemoveAbilityRecordObserver(AbilityRecordObserver *observer)
{
    if (identity_ == nullptr) {
        return PARAM_CHECK_ERROR;
    }
    Request request = {
        .msgId = REMOVE_ABILITY_RECORD_OBSERVER,
        .len = 0,
        .data = nullptr,
        .msgValue = reinterpret_cast<uint32>(observer),
    };

    return SendRequestToAms(request);
}

MissionInfoList *AbilityMsClient::GetMissionInfos(uint32_t maxNum) const
{
    if (!Initialize()) {
        return nullptr;
    }
    return static_cast<MissionInfoList *>(amsProxy_->GetMissionInfos(maxNum));
}

void AbilityMsClient::SetServiceIdentity(const Identity *identity)
{
    identity_ = identity;
}
} // namespace AbilitySlite
} // namespace OHOS
