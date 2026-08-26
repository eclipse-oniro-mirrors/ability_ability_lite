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

#include "ability_record_manager.h"
#include "aafwk_event_error_id.h"
#include "aafwk_event_error_code.h"
#include "ability_errors.h"
#include "ability_inner_message.h"
#include "ability_lock_guard.h"
#include "ability_record.h"
#include "ability_record_observer_manager.h"
#include "ability_service_interface.h"
#include "ability_thread_loader.h"
#include "abilityms_log.h"
#include "ability_manager_inner.h"
#include "bms_helper.h"
#include "bundle_manager.h"
#include "cmsis_os2.h"
#ifdef OHOS_DMS_ENABLED
#include "dmsfwk_interface.h"
#endif
#include "js_ability_thread.h"
#ifdef OHOS_DMS_ENABLED
#include "samgr_lite.h"
#endif
#include "slite_ability.h"
#include "utils.h"
#include "want.h"

using namespace OHOS::ACELite;

namespace OHOS {
namespace AbilitySlite {
constexpr int32_t QUEUE_LENGTH = 32;
constexpr int32_t APP_TASK_PRI = 25;

AbilityRecordManager::AbilityRecordManager() = default;

AbilityRecordManager::~AbilityRecordManager()
{
    DeleteRecordInfo(LAUNCHER_TOKEN);
}

void AbilityRecordManager::StartLauncher()
{
    AbilityRecord *launcherRecord = abilityList_.Get(LAUNCHER_TOKEN);
    if (launcherRecord != nullptr) {
        return;
    }

#ifndef _MINI_MULTI_TASKS_
    auto record = new AbilityRecord();
    record->SetAppName(LAUNCHER_BUNDLE_NAME);
    record->token = LAUNCHER_TOKEN;
    record->isNativeApp = true;
    record->state = SCHEDULE_FOREGROUND;
    record->taskId = osThreadGetId();
    abilityList_.Add(record);
    (void)ScheduleLifecycleInner(record, SLITE_STATE_FOREGROUND);
#else // define _MINI_MULTI_TASKS_
    Want *want = static_cast<Want *>(AdapterMalloc(sizeof(Want)));
    if (want == nullptr) {
        return;
    }
    want->data = nullptr;
    want->dataLength = 0;
    want->element = nullptr;
    want->appPath = nullptr;
    want->actions = nullptr;
    want->entities = nullptr;
    ElementName elementName = {};
    SetElementBundleName(&elementName, BMSHelper::GetInstance().GetStartupBundleName());
    SetWantElement(want, elementName);
    ClearElement(&elementName);
    StartAbility(want);
    ClearWant(want);
    AdapterFree(want);
#endif
}

int32_t AbilityRecordManager::StartAbility(const AbilityRecord *record)
{
    if (record == nullptr) {
        return PARAM_NULL_ERROR;
    }
    Want *want = static_cast<Want *>(AdapterMalloc(sizeof(Want)));
    if (want == nullptr) {
        return PARAM_NULL_ERROR;
    }
    want->data = nullptr;
    want->dataLength = 0;
    want->element = nullptr;
    want->appPath = nullptr;
    want->actions = nullptr;
    want->entities = nullptr;
    ElementName elementName = {};
    if (record != nullptr) {
        want->data = Utils::Memdup(record->abilityData->wantData, record->abilityData->wantDataSize);
        want->dataLength = record->abilityData->wantDataSize;
        want->appPath = Utils::Strdup(record->appPath);
        SetElementBundleName(&elementName, record->appName);
    }
    SetWantElement(want, elementName);
    ClearElement(&elementName);

    auto ret = StartAbility(want);
    ClearWant(want);
    if (ret != ERR_OK) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "start ability failed [%{public}d]", ret);
    }
    AdapterFree(want);
    return ret;
}

bool AbilityRecordManager::IsLauncher(const char *bundleName)
{
    size_t len = strlen(bundleName);
    const char* suffix = ".launcher";
    size_t suffixLen = strlen(suffix);
    if (len < suffixLen) {
        return false;
    }
    return (strcmp(bundleName + len - suffixLen, suffix) == 0);
}

int32_t AbilityRecordManager::StartRemoteAbility(const Want *want)
{
#ifdef OHOS_DMS_ENABLED
    IUnknown *iUnknown = SAMGR_GetInstance()->GetFeatureApi(DISTRIBUTED_SCHEDULE_SERVICE, DMSLITE_FEATURE);
    if (iUnknown == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Failed to get distributed schedule service.");
        return EC_INVALID;
    }
    DmsProxy *dmsInterface = nullptr;
    int32_t retVal = iUnknown->QueryInterface(iUnknown, DEFAULT_VERSION, (void **) &dmsInterface);
    if (retVal != EC_SUCCESS) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Failed to get DMS interface retVal: [%{public}d]", retVal);
        return EC_INVALID;
    }
    AbilityRecord *record = abilityList_.GetByTaskId(curTask_);
    if (record == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Failed to get record by taskId.");
        return PARAM_NULL_ERROR;
    }
    const char *callerBundleName = record->GetAppName();
    if (callerBundleName == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Failed to get callerBundleName.");
        return PARAM_NULL_ERROR;
    }

    CallerInfo callerInfo = {
        .uid = 0,
        .bundleName = OHOS::Utils::Strdup(callerBundleName)
    };
    retVal = dmsInterface->StartRemoteAbility(want, &callerInfo, nullptr);

    HILOG_INFO(HILOG_MODULE_AAFWK, "StartRemoteAbility retVal: [%{public}d]", retVal);
    AdapterFree(callerInfo.bundleName);
    return retVal;
#else
    return PARAM_NULL_ERROR;
#endif
}

int32_t AbilityRecordManager::StartAbility(const Want *want)
{
    if (isAppScheduling_) {
        return AddAbilityOperation(START_ABILITY, want, 0);
    }
    isAppScheduling_ = true;
    if (want == nullptr || want->element == nullptr) {
        isAppScheduling_ = false;
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability Service wanted element is null");
        return PARAM_NULL_ERROR;
    }
    char *bundleName = want->element->bundleName;
    if (bundleName == nullptr) {
        isAppScheduling_ = false;
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability Service wanted bundleName is null");
        return PARAM_NULL_ERROR;
    }

#ifdef OHOS_DMS_ENABLED
    if (want->element->deviceId != nullptr && *(want->element->deviceId) != '\0') {
        // deviceId is set
        isAppScheduling_ = false;
        return StartRemoteAbility(want);
    }
#endif

#ifdef _MINI_MULTI_TASKS_
    AbilityRecord *abilityRecord = abilityList_.Get(bundleName);
    if (abilityRecord != nullptr) {
        auto topRecord = abilityList_.GetTopAbility();
        if (topRecord == abilityRecord) {
            isAppScheduling_ = false;
            return ERR_OK;
        }
        if ((abilityRecord->isNativeApp == false) && !CheckResponse(bundleName)) {
            HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability Service CheckResponse Failed.");
            isAppScheduling_ = false;
            return PARAM_CHECK_ERROR;
        }
        abilityRecord->SetWantData(want->data, want->dataLength);
        if (want->mission != UINT32_MAX) {
            abilityRecord->mission = want->mission;
        }
        abilityList_.MoveToTop(abilityRecord->token);
        if (NeedToBeTerminated(topRecord->appName)) {
            topRecord->isTerminated = true;
        }
        return SendMsgToAbilityThread(SLITE_STATE_BACKGROUND, topRecord);
    }
#endif

    auto *info = static_cast<AbilitySvcInfo *>(AdapterMalloc(sizeof(AbilitySvcInfo)));
    if (info == nullptr) {
        isAppScheduling_ = false;
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability Service AbilitySvcInfo is null");
        return PARAM_NULL_ERROR;
    }
    uint8_t queryRet = BMSHelper::GetInstance().QueryAbilitySvcInfo(want, info);
    if (queryRet != ERR_OK) {
        isAppScheduling_ = false;
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability BMS Helper return abilitySvcInfo failed");
        AdapterFree(info);
        return PARAM_CHECK_ERROR;
    }

    info->data = OHOS::Utils::Memdup(want->data, want->dataLength);
    info->dataLength = want->dataLength;
    info->mission = want->mission;
    auto ret = StartAbility(info);
    BMSHelper::GetInstance().ClearAbilitySvcInfo(info);
    AdapterFree(info->data);
    AdapterFree(info);
    return ret;
}

#ifndef _MINI_MULTI_TASKS_
void AbilityRecordManager::UpdateRecord(AbilitySvcInfo *info)
{
    if (info == nullptr) {
        return;
    }
    AbilityRecord *record = abilityList_.Get(info->bundleName);
    if (record == nullptr) {
        return;
    }
    if (record->token != LAUNCHER_TOKEN) {
        return;
    }
    record->SetWantData(info->data, info->dataLength);
}
#endif // _MINI_MULTI_TASKS_

int32_t AbilityRecordManager::StartAbility(AbilitySvcInfo *info)
{
    if ((info == nullptr) || (info->bundleName == nullptr) || (strlen(info->bundleName) == 0)) {
        isAppScheduling_ = false;
        return PARAM_NULL_ERROR;
    }
    HILOG_INFO(HILOG_MODULE_AAFWK, "StartAbility");

    auto topRecord = abilityList_.GetTopAbility();
#ifndef _MINI_MULTI_TASKS_
    if ((topRecord == nullptr) || (topRecord->appName == nullptr)) {
        isAppScheduling_ = false;
        HILOG_ERROR(HILOG_MODULE_AAFWK, "StartAbility top null.");
        return PARAM_NULL_ERROR;
    }
    uint16_t topToken = topRecord->token;
    //  start launcher
    if (IsLauncher(info->bundleName)) {
        UpdateRecord(info);
        if (topToken != LAUNCHER_TOKEN && topRecord->state != SCHEDULE_BACKGROUND) {
            HILOG_INFO(HILOG_MODULE_AAFWK, "Change Js app to background.");
            (void)ScheduleLifecycleInner(topRecord, SLITE_STATE_BACKGROUND);
        } else {
            (void) SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_FOREGROUND);
        }
        return ERR_OK;
    }

    if ((info->isNativeApp == false) && !CheckResponse(info->bundleName)) {
        isAppScheduling_ = false;
        return PARAM_CHECK_ERROR;
    }

    // start js app
    if (topRecord->state != SCHEDULE_STOP && topRecord->token != LAUNCHER_TOKEN) {
        // start app is top
        if (strcmp(info->bundleName, topRecord->appName) == 0) {
            if (topRecord->state == SCHEDULE_BACKGROUND) {
                HILOG_INFO(HILOG_MODULE_AAFWK, "StartAbility Resume app when background.");
                (void) SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_BACKGROUND);
                return ERR_OK;
            }
            HILOG_INFO(HILOG_MODULE_AAFWK, "Js app already started or starting.");
        } else {
            // js to js
            HILOG_INFO(HILOG_MODULE_AAFWK, "Terminate pre js app when js to js");
            TerminateAbility(topRecord->token);
            pendingToken_ = GenerateToken();
        }
    }

    // application has not been launched and then to check priority and permission.
    info->isNativeApp = false;
    return PreCheckStartAbility(*info);
#else
    if (topRecord == nullptr) {
        if (strcmp(info->bundleName, BMSHelper::GetInstance().GetStartupBundleName()) != 0) {
            HILOG_ERROR(HILOG_MODULE_AAFWK, "first ability should be launcher.");
            return PARAM_CHECK_ERROR;
        }
        // start launcher when boot
        return PreCheckStartAbility(*info);
    }

    if ((info->isNativeApp == false) && !CheckResponse(info->bundleName)) {
        return PARAM_CHECK_ERROR;
    }

    // the topAbility needs to be transferred to background
    // start topAbility
    if (strcmp(info->bundleName, topRecord->appName) == 0) {
        if (topRecord->state == SCHEDULE_STOP) {
            CreateAppTask(const_cast<AbilityRecord *>(topRecord));
        } else {
            isAppScheduling_ = false;
        }
        return ERR_OK;
    }
    if (NeedToBeTerminated(topRecord->appName)) {
        topRecord->isTerminated = true;
    }
    (void) SendMsgToAbilityThread(SLITE_STATE_BACKGROUND, topRecord);
    pendingToken_ = GenerateToken();

    // application has not been launched and then to check priority and permission.
    return PreCheckStartAbility(*info);
#endif
}

int32_t AbilityRecordManager::TerminateAbility(uint16_t token)
{
    if (isAppScheduling_) {
        return AddAbilityOperation(TERMINATE_ABILITY, nullptr, token);
    }
    isAppScheduling_ = true;
    return TerminateAbility(token, nullptr);
}

int32_t AbilityRecordManager::TerminateMission(uint32_t mission)
{
    if (isAppScheduling_) {
        return AddAbilityOperation(TERMINATE_MISSION, nullptr, mission);
    }

    AbilityRecord *topRecord = const_cast<AbilityRecord *>(abilityList_.GetTopAbility());
    if (topRecord == nullptr) {
        APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_STOP_NO_ABILITY_RUNNING);
        return PARAM_NULL_ERROR;
    }

    List<uint32_t> list;
    abilityList_.GetAbilityList(mission, list);
    for (auto node = list.Begin(); node != list.End(); node = node->next_) {
        uint16_t token = static_cast<uint32_t>(node->value_);
        if (token != topRecord->token) {
            TerminateAbility(token);
        }
    }

    return ERR_OK;
}

int32_t AbilityRecordManager::TerminateAbility(uint16_t token, const Want* want)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "TerminateAbility [%{public}u]", token);
    AbilityRecord *topRecord = const_cast<AbilityRecord *>(abilityList_.GetTopAbility());
    if (topRecord == nullptr) {
        isAppScheduling_ = false;
        APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_STOP_NO_ABILITY_RUNNING);
        return PARAM_NULL_ERROR;
    }
    uint16_t topToken = topRecord->token;
#ifndef _MINI_MULTI_TASKS_
    if (token == LAUNCHER_TOKEN) {
        // if js is in background, the launcher goes back to background and js goes to active
        isAppScheduling_ = false;
        if (topToken != token && topRecord->state == SCHEDULE_BACKGROUND) {
            HILOG_INFO(HILOG_MODULE_AAFWK, "Resume Js app [%{public}u]", topToken);
            return SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_BACKGROUND);
        }
        return ERR_OK;
    }

    if (token != topToken) {
        isAppScheduling_ = false;
        APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_STOP_UNKNOWN_ABILITY_TOKEN);
        DeleteRecordInfo(token);
        return -1;
    }
    topRecord->isTerminated = true;
    // TerminateAbility top js
    return ScheduleLifecycleInner(topRecord, SLITE_STATE_BACKGROUND);
#else
    // 1. only launcher in the ability stack
    if (abilityList_.Size() == 1 && AbilityList::IsPermanentAbility(*topRecord)) {
        isAppScheduling_ = false;
        return ERR_OK;
    }
    // 2. terminate non-top ability
    if (token != topToken) {
        AbilityRecord* abilityRecord = abilityList_.Get(token);
        if ((abilityRecord == nullptr) || (AbilityList::IsPermanentAbility(*abilityRecord))) {
            isAppScheduling_ = false;
            return PARAM_CHECK_ERROR;
        }
        APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_STOP_UNKNOWN_ABILITY_TOKEN);
        DeleteRecordInfo(token);
        isAppScheduling_ = false;
        return ERR_OK;
    }
    // 3. terminate top ability
    abilityList_.PopAbility();
    AbilityRecord *newTopRecord = const_cast<AbilityRecord *>(abilityList_.GetTopAbility());
    if (newTopRecord == nullptr) {
        isAppScheduling_ = false;
        APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_STOP_NO_ABILITY_RUNNING);
        return PARAM_NULL_ERROR;
    }

    if (!AbilityList::IsPermanentAbility(*topRecord)) {
        topRecord->isTerminated = true;
        abilityList_.Add(topRecord);
    } else {
        // launcher will not pop ability stack
        abilityList_.PopAbility();
        abilityList_.Add(topRecord);
        abilityList_.Add(newTopRecord);
    }
    if (want != nullptr) {
        if (newTopRecord->abilityData != nullptr) {
            AdapterFree(newTopRecord->abilityData->wantData);
        }
        if (want->data != nullptr) {
            newTopRecord->SetWantData(want->data, want->dataLength);
        }
        HILOG_INFO(HILOG_MODULE_AAFWK, "Terminate ability with want, dataLength is %{public}u", want->dataLength);
    } else {
        HILOG_INFO(HILOG_MODULE_AAFWK, "Terminate ability with no want");
    }

    // TerminateAbility top js
    pendingToken_ = newTopRecord->token;
    return SendMsgToAbilityThread(SLITE_STATE_BACKGROUND, topRecord);
#endif
}

int32_t AbilityRecordManager::ForceStopBundle(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "ForceStopBundle [%{public}u]", token);
    if (isAppScheduling_) {
        return AddAbilityOperation(TERMINATE_APP, nullptr, token);
    }
    isAppScheduling_ = true;

#ifndef _MINI_MULTI_TASKS_
    if (token == LAUNCHER_TOKEN) {
        HILOG_INFO(HILOG_MODULE_AAFWK, "Launcher does not support force stop.");
        isAppScheduling_ = false;
        return ERR_OK;
    }

    // free js mem and delete the record
    if (ForceStopBundleInner(token) != ERR_OK) {
        return PARAM_CHECK_ERROR;
    }

    // active the launcher
    AbilityRecord *launcherRecord = abilityList_.Get(LAUNCHER_TOKEN);
    if (launcherRecord == nullptr) {
        return PARAM_NULL_ERROR;
    }
    if (launcherRecord->state != SCHEDULE_FOREGROUND) {
        return SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_FOREGROUND);
    }
#else
    AbilityRecord* topRecord = abilityList_.GetTopAbility();
    if (topRecord->token != token) {
        return TerminateAbility(token, nullptr);
    }

    HILOG_INFO(HILOG_MODULE_AAFWK, "force stop top ability.");
    topRecord->isTerminated = true;
    OnDestroyDone(token);
#endif
    return ERR_OK;
}

int32_t AbilityRecordManager::TerminateAll(const char *excludedBundleName)
{
    if (isAppScheduling_) {
        Want want;
        if (excludedBundleName == nullptr) {
            want.data = nullptr;
            want.dataLength = 0;
        } else {
            want.data = reinterpret_cast<void *>(const_cast<char *>(excludedBundleName));
            want.dataLength = strlen(excludedBundleName) + 1;
        }
        want.element = nullptr;
        want.appPath = nullptr;
        return AddAbilityOperation(TERMINATE_ALL, &want, 0);
    }
    isAppScheduling_ = true;
    int32_t ret = abilityList_.PopAllAbility(excludedBundleName);
    isAppScheduling_ = false;
    return ret;
}

int32_t AbilityRecordManager::ForceStop(const Want *want)
{
    if (isAppScheduling_) {
        return AddAbilityOperation(TERMINATE_APP_BY_BUNDLENAME, want, 0);
    }
    isAppScheduling_ = true;
    if (want == nullptr
        || want->element == nullptr
        || want->element->bundleName == nullptr) {
        isAppScheduling_ = false;
        return PARAM_NULL_ERROR;
    }

#ifndef _MINI_MULTI_TASKS_
    // stop Launcher
    if (IsLauncher(want->element->bundleName)) {
        return TerminateAbility(0, nullptr);
    }
#endif
    // stop app
    AbilityRecord *terminateRecord = abilityList_.Get(want->element->bundleName);
    if (terminateRecord == nullptr) {
        isAppScheduling_ = false;
        HILOG_ERROR(HILOG_MODULE_AAFWK, "ForceStop, The specified ability is not found.");
        return PARAM_CHECK_ERROR;
    }
    HILOG_INFO(HILOG_MODULE_AAFWK, "ForceStop [%{public}u]", terminateRecord->token);
    return TerminateAbility(terminateRecord->token, want);
}

int32_t AbilityRecordManager::ForceStopBundleInner(uint16_t token)
{
    return ERR_OK;
}

int32_t AbilityRecordManager::PreCheckStartAbility(const AbilitySvcInfo &info)
{
#ifndef _MINI_MULTI_TASKS_
    if (info.path == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "PreCheckStartAbility path is null.");
        return PARAM_NULL_ERROR;
    }
    auto curRecord = abilityList_.Get(info.bundleName);
    if (curRecord != nullptr) {
        if (curRecord->state == SCHEDULE_FOREGROUND) {
            HILOG_ERROR(HILOG_MODULE_AAFWK, "PreCheckStartAbility current state active.");
        } else if (curRecord->state == SCHEDULE_BACKGROUND) {
            SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_BACKGROUND);
        }
        return ERR_OK;
    }
    auto record = new AbilityRecord();
    record->SetAppName(info.bundleName);
    record->SetAppPath(info.path);
    record->SetWantData(info.data, info.dataLength);
    record->isNativeApp = BMSHelper::GetInstance().IsNativeApp(info.bundleName);
    record->state = SCHEDULE_STOP;
    if (pendingToken_ != 0) {
        record->token = pendingToken_;
        pendingRecord = record;
    } else {
        record->token = GenerateToken();
        abilityList_.Add(record);
    }
    if (pendingToken_ == 0 && CreateAppTask(record) != ERR_OK) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "CheckResponse CreateAppTask fail");
        abilityList_.Erase(record->token);
        delete record;
        return CREATE_APPTASK_ERROR;
    }
#else
    if ((info.path == nullptr) && !(info.isNativeApp)) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "PreCheckStartAbility path is null.");
        return PARAM_NULL_ERROR;
    }
    auto curRecord = abilityList_.Get(info.bundleName);
    AbilityRecord *record = nullptr;
    if (curRecord != nullptr) {
        if (curRecord->state != SCHEDULE_STOP) {
            HILOG_ERROR(HILOG_MODULE_AAFWK, "PreCheckStartAbility current state active.");
        } else {
            // update ability stack and move the ability to the top of ability stack
            abilityList_.MoveToTop(curRecord->token);
            pendingToken_ = curRecord->token;
            return ERR_OK;
        }
    } else {
        record = new AbilityRecord();
        if (pendingToken_ != 0) {
            record->token = pendingToken_;
        } else {
            record->token = GenerateToken();
        }
        record->SetAppName(info.bundleName);
        record->SetAppPath(info.path);
        record->SetWantData(info.data, info.dataLength);
        record->state = SCHEDULE_STOP;
        record->isNativeApp = info.isNativeApp;
        record->mission = info.mission;
        if (record->mission == UINT32_MAX) {
            record->mission = GenerateMission();
        }
        abilityList_.Add(record);
    }

    if (pendingToken_ == 0 && CreateAppTask(record) != ERR_OK) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "CheckResponse CreateAppTask fail");
        abilityList_.Erase(record->token);
        delete record;
        return CREATE_APPTASK_ERROR;
    }
#endif
    return ERR_OK;
}

bool AbilityRecordManager::CheckResponse(const char *bundleName)
{
    StartCheckFunc callBackFunc = GetAbilityCallback();
    if (callBackFunc == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "calling ability callback failed: null");
        return true;
    }
    int32_t ret = (*callBackFunc)(bundleName);
    if (ret != ERR_OK) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "calling ability callback failed: check");
        return false;
    }
    return true;
}

int32_t AbilityRecordManager::CreateAppTask(AbilityRecord *record)
{
    if ((record == nullptr) || (record->appName == nullptr)) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "CreateAppTask fail: null");
        return PARAM_NULL_ERROR;
    }

    if (record->isNativeApp) {
        record->abilityThread =
            AbilityThreadLoader::GetInstance().CreateAbilityThread(AbilityThreadCreatorType::NATIVE_CREATOR);
    } else {
        record->abilityThread =
            AbilityThreadLoader::GetInstance().CreateAbilityThread(AbilityThreadCreatorType::JS_CREATOR);
    }

    if (record->abilityThread == nullptr) {
        return MEMORY_MALLOC_ERROR;
    }
    int32_t ret = record->abilityThread->InitAbilityThread(record);
    if (ret != ERR_OK) {
        delete record->abilityThread;
        record->abilityThread = nullptr;
        return ret;
    }
    record->taskId = record->abilityThread->GetAppTaskId();
    record->jsAppQueueId = record->abilityThread->GetMessageQueueId();
    record->state = SCHEDULE_STOP;

#ifndef _MINI_MULTI_TASKS_
    APP_EVENT(MT_ACE_APP_START);
    abilityList_.Add(record);
    if (nativeAbility_ != nullptr) {
        if (SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_BACKGROUND) != 0) {
            APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_START_LAUNCHER_EXIT_FAILED);
            HILOG_INFO(HILOG_MODULE_AAFWK, "CreateAppTask Fail to hide launcher");
            return SCHEDULER_LIFECYCLE_ERROR;
        }
    } else {
        SchedulerLifecycle(record->token, SLITE_STATE_INITIAL);
    }
#else
    APP_EVENT(MT_ACE_APP_START);
    SchedulerLifecycle(record->token, SLITE_STATE_INITIAL);
#endif
    return ERR_OK;
}

uint16_t AbilityRecordManager::GenerateToken()
{
    static uint16_t token = LAUNCHER_TOKEN;
    if (token == UINT16_MAX - 1) {
        token = LAUNCHER_TOKEN;
    }

#ifndef _MINI_MULTI_TASKS_
    return ++token;
#else
    return token++;
#endif
}

uint32_t AbilityRecordManager::GenerateMission()
{
    static uint32_t mission = 0;
    if (mission == UINT32_MAX) {
        mission = 0;
    }
    return mission++;
}

void AbilityRecordManager::DeleteRecordInfo(uint16_t token)
{
    AbilityRecord *record = abilityList_.Get(token);
    if (record == nullptr) {
        return;
    }
    DeleteAbilityThread(record);
    // record app info event when stop app
    RecordAbiityInfoEvt(record->GetAppName());
    abilityList_.Erase(token);
    AbilityRecordObserverManager::GetInstance().NotifyAbilityRecordCleanup(record->appName);
    delete record;
}

void AbilityRecordManager::DeleteAbilityThread(AbilityRecord *record)
{
    if (record->abilityThread != nullptr) {
        record->abilityThread->ReleaseAbilityThread();
        delete record->abilityThread;
        record->abilityThread = nullptr;
    }
    // free all JS native memory after exiting it
    // CleanTaskMem(taskId)
}

void AbilityRecordManager::OnCreateDone(uint16_t token)
{
    SetAbilityStateAndNotify(token, SCHEDULE_INITED);
    SchedulerLifecycle(token, SLITE_STATE_FOREGROUND);
}

void AbilityRecordManager::OnForegroundDone(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "OnForegroundDone [%{public}u]", token);
    SetAbilityStateAndNotify(token, SCHEDULE_FOREGROUND);
    auto topRecord = const_cast<AbilityRecord *>(abilityList_.GetTopAbility());
    if (topRecord == nullptr) {
        return;
    }
    HILOG_INFO(HILOG_MODULE_AAFWK, "The number of tasks in the stack is %{public}u.", abilityList_.Size());

#ifndef _MINI_MULTI_TASKS_
    // the launcher foreground
    if (token == LAUNCHER_TOKEN) {
        if (nativeAbility_ == nullptr || nativeAbility_->GetState() != SLITE_STATE_FOREGROUND) {
            HILOG_ERROR(HILOG_MODULE_AAFWK, "native ability is in wrong state : %{public}d",
                nativeAbility_->GetState());
            return;
        }
        if (topRecord->token != LAUNCHER_TOKEN) {
            int abilityState = SLITE_STATE_UNINITIALIZED;
            if (topRecord->state == SCHEDULE_FOREGROUND) {
                HILOG_ERROR(HILOG_MODULE_AAFWK,
                    "js is in foreground state, native state is %{public}d", abilityState);
                OnDestroyDone(topRecord->token);
                return;
            }
            if (topRecord->state != SCHEDULE_BACKGROUND) {
                APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_START_LAUNCHER_EXIT_FAILED);
                HILOG_ERROR(HILOG_MODULE_AAFWK,
                    "Active launcher js bg fail, native state is %{public}d", abilityState);
                DeleteRecordInfo(topRecord->token);
            } else if (topRecord->isTerminated) {
                (void)ScheduleLifecycleInner(topRecord, SLITE_STATE_UNINITIALIZED);
            }
        }
        return;
    }
#endif // _MINI_MULTI_TASKS_

    // the js app active
    if (topRecord->token == token) {
        APP_EVENT(MT_ACE_APP_ACTIVE);
    }
}

void AbilityRecordManager::OnBackgroundDone(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "OnBackgroundDone [%{public}u]", token);
    SetAbilityStateAndNotify(token, SCHEDULE_BACKGROUND);

#ifndef _MINI_MULTI_TASKS_
    const AbilityRecord *topRecord = abilityList_.GetTopAbility();
    if (topRecord == nullptr) {
        return;
    }
    // the js background
    if (token != LAUNCHER_TOKEN) {
        if (topRecord->token == token) {
            APP_EVENT(MT_ACE_APP_BACKGROUND);
            (void) SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_FOREGROUND);
        }
        return;
    }
    // the launcher background
    if (topRecord->token != LAUNCHER_TOKEN) {
        if (topRecord->state == SCHEDULE_STOP) {
            (void)ScheduleLifecycleInner(topRecord, SLITE_STATE_INITIAL);
        } else {
            (void)ScheduleLifecycleInner(topRecord, SLITE_STATE_FOREGROUND);
        }
        if (GetCleanAbilityDataFlag()) {
            HILOG_INFO(HILOG_MODULE_AAFWK, "OnBackgroundDone clean launcher record data");
            AbilityRecord *record = abilityList_.Get(token);
            record->SetWantData(nullptr, 0);
            SetCleanAbilityDataFlag(false);
        }
        return;
    }
#else
    AbilityRecord *record = abilityList_.Get(token);
    if (record == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "token is not found");
        return;
    }

    if ((!record->isTerminated) && (record->abilitySavedData == nullptr)) {
        record->abilitySavedData = new AbilitySavedData();
    }
    if (record->abilitySavedData != nullptr) {
        record->abilitySavedData->Reset();
    }

    SendMsgToAbilityThread(SLITE_STATE_UNINITIALIZED, record);
#endif
}

void AbilityRecordManager::OnDestroyDone(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "OnDestroyDone [%{public}u]", token);
    SetAbilityStateAndNotify(token, SCHEDULE_STOP);
#ifndef _MINI_MULTI_TASKS_
    // the launcher destroy
    if (token == LAUNCHER_TOKEN) {
        return;
    }
    auto topRecord = abilityList_.GetTopAbility();
    if ((topRecord == nullptr) || (topRecord->token != token)) {
        DeleteRecordInfo(token);
        return;
    }
    APP_EVENT(MT_ACE_APP_STOP);
    DeleteRecordInfo(token);

    // no pending token
    if (pendingToken_ == 0) {
        (void) SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_FOREGROUND);
        return;
    }

    // start pending token
    auto record = pendingRecord;
    if (record == nullptr) {
        return;
    }
    if (CreateAppTask(record) != ERR_OK) {
        delete record;
        pendingRecord = nullptr;
        (void) SchedulerLifecycle(LAUNCHER_TOKEN, SLITE_STATE_FOREGROUND);
    }
    pendingToken_ = 0;
    pendingRecord = nullptr;
#else
    auto onDestroyRecord = abilityList_.Get(token);
    if (onDestroyRecord == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "token is not found");
        return;
    }
    // 1. ability is terminated and pop out ability stack
    if (onDestroyRecord->isTerminated) {
        APP_EVENT(MT_ACE_APP_STOP);
        DeleteRecordInfo(token);
    } else {
        // 2. ability is transferred to SCHEDULE_STOP state and still keep in the ability stack
        DeleteAbilityThread(onDestroyRecord);
    }

    // start pending token
    if (pendingToken_ != 0) {
        auto record = abilityList_.Get(pendingToken_);
        if (record == nullptr) {
            return;
        }
        if (CreateAppTask(record) != ERR_OK) {
            abilityList_.Erase(pendingToken_);
            delete record;
            auto topRecord = abilityList_.GetTopAbility();
            if (topRecord == nullptr) {
                HILOG_ERROR(HILOG_MODULE_AAFWK, "record stack is empty");
                return;
            }
            isAppScheduling_ = false;
            StartAbility(topRecord);
        }
        pendingToken_ = 0;
    } else {
        // start top ability
        auto topAbilityRecord = abilityList_.GetTopAbility();
        if (pendingToken_ == 0 && CreateAppTask(topAbilityRecord) != ERR_OK) {
            abilityList_.Erase(topAbilityRecord->token);
            delete topAbilityRecord;
            isAppScheduling_ = false;
        }
    }
#endif
}

int32_t AbilityRecordManager::SchedulerLifecycle(uint64_t token, int32_t state)
{
    AbilityRecord *record = abilityList_.Get(token);
    if (record == nullptr) {
        return PARAM_NULL_ERROR;
    }

#ifndef _MINI_MULTI_TASKS_
    return ScheduleLifecycleInner(record, state);
#else
    return SendMsgToAbilityThread(state, record);
#endif
}

void AbilityRecordManager::SetAbilityStateAndNotify(uint64_t token, int32_t state)
{
    AbilityRecord *record = abilityList_.Get(token);
    if (record == nullptr) {
        return;
    }
    record->state = state;
    AbilityRecordObserverManager::GetInstance().NotifyAbilityRecordStateChanged(
        AbilityRecordStateData(record->appName, static_cast<AbilityRecordState>(state)));
}

#ifndef _MINI_MULTI_TASKS_
int32_t AbilityRecordManager::ScheduleLifecycleInner(const AbilityRecord *record, int32_t state)
{
    if (record == nullptr) {
        return PARAM_NULL_ERROR;
    }
    // dispatch js life cycle
    if (record->token != LAUNCHER_TOKEN) {
        (void) SendMsgToAbilityThread(state, record);
        return ERR_OK;
    }
    // dispatch native life cycle
    if (nativeAbility_ == nullptr) {
        return PARAM_NULL_ERROR;
    }
    // malloc want memory and release after use
    Want *info = static_cast<Want *>(AdapterMalloc(sizeof(Want)));
    if (info == nullptr) {
        return MEMORY_MALLOC_ERROR;
    }
    info->element = nullptr;
    info->data = nullptr;
    info->dataLength = 0;
    info->appPath = nullptr;
    info->actions = nullptr;
    info->entities = nullptr;

    ElementName elementName = {};
    SetElementBundleName(&elementName, LAUNCHER_BUNDLE_NAME);
    SetWantElement(info, elementName);
    ClearElement(&elementName);
    if (record->abilityData != nullptr) {
        SetWantData(info, record->abilityData->wantData, record->abilityData->wantDataSize);
    } else {
        SetWantData(info, nullptr, 0);
    }
    SchedulerAbilityLifecycle(nativeAbility_, *info, state);
    ClearWant(info);
    AdapterFree(info);
    return ERR_OK;
}
#endif

void AbilityRecordManager::SchedulerAbilityLifecycle(SliteAbility *ability, const Want &want, int32_t state)
{
    if (ability == nullptr) {
        return;
    }
    switch (state) {
        case SLITE_STATE_FOREGROUND: {
            ability->OnForeground(want);
            break;
        }
        case SLITE_STATE_BACKGROUND: {
            ability->OnBackground();
            break;
        }
        default: {
            break;
        }
    }
    return;
}

int32_t AbilityRecordManager::SchedulerLifecycleDone(uint64_t token, int32_t state)
{
    switch (state) {
        case SLITE_STATE_INITIAL: {
            OnCreateDone(token);
            break;
        }
        case SLITE_STATE_FOREGROUND: {
            OnForegroundDone(token);
            isAppScheduling_ = false;
            RunOperation();
            break;
        }
        case SLITE_STATE_BACKGROUND: {
            OnBackgroundDone(token);
            break;
        }
        case SLITE_STATE_UNINITIALIZED: {
            OnDestroyDone(token);
            break;
        }
        default: {
            break;
        }
    }
    return ERR_OK;
}

int32_t AbilityRecordManager::SendMsgToAbilityThread(int32_t state, const AbilityRecord *record)
{
    if (record == nullptr || record->abilityThread == nullptr) {
        return PARAM_NULL_ERROR;
    }

    SliteAbilityInnerMsg innerMsg;
    switch (state) {
        case SLITE_STATE_INITIAL:
            innerMsg.msgId = SliteAbilityMsgId::CREATE;
            innerMsg.want = CreateWant(record);
            if (!record->isTerminated) {
                innerMsg.abilitySavedData = record->abilitySavedData;
            }
            break;
        case SLITE_STATE_FOREGROUND:
            innerMsg.msgId = SliteAbilityMsgId::FOREGROUND;
            innerMsg.want = CreateWant(record);
            break;
        case SLITE_STATE_BACKGROUND:
            innerMsg.msgId = SliteAbilityMsgId::BACKGROUND;
            break;
        case SLITE_STATE_UNINITIALIZED:
            innerMsg.msgId = SliteAbilityMsgId::DESTROY;
            if (!record->isTerminated) {
                innerMsg.abilitySavedData = record->abilitySavedData;
            }
            break;
        default:
            innerMsg.msgId = (SliteAbilityMsgId)state;
            break;
    }
    innerMsg.abilityThread = record->abilityThread;
    return record->abilityThread->SendScheduleMsgToAbilityThread(innerMsg);
}

Want *AbilityRecordManager::CreateWant(const AbilityRecord *record)
{
    Want *want = static_cast<Want *>(AdapterMalloc(sizeof(Want)));
    want->element = nullptr;
    want->data = nullptr;
    want->dataLength = 0;
    want->appPath = OHOS::Utils::Strdup(record->appPath);
    want->mission = record->mission;
    want->actions = nullptr;
    want->entities = nullptr;
    ElementName elementName = {};
    SetElementBundleName(&elementName, record->appName);
    SetWantElement(want, elementName);
    if (record->abilityData != nullptr) {
        SetWantData(want, record->abilityData->wantData, record->abilityData->wantDataSize);
    }
    ClearElement(&elementName);
    return want;
}

bool AbilityRecordManager::NeedToBeTerminated(const char *bundleName)
{
    return BMSHelper::GetInstance().IsTemporaryBundleName(bundleName);
}

ElementName *AbilityRecordManager::GetTopAbility()
{
    auto topRecord = const_cast<AbilityRecord *>(abilityList_.GetTopAbility());
    if (topRecord == nullptr) {
        return nullptr;
    }
    ElementName *element = reinterpret_cast<ElementName *>(AdapterMalloc(sizeof(ElementName)));
    if (element == nullptr || memset_s(element, sizeof(ElementName), 0, sizeof(ElementName)) != EOK) {
        AdapterFree(element);
        return nullptr;
    }

    // case js active or background when launcher not active
    if (topRecord->state == SCHEDULE_FOREGROUND || topRecord->state == SCHEDULE_BACKGROUND) {
        SetElementBundleName(element, topRecord->appName);
    }
    return element;
}

MissionInfoList *AbilityRecordManager::GetMissionInfos(uint32_t maxNum) const
{
    return abilityList_.GetMissionInfos(maxNum);
}

void AbilityRecordManager::setNativeAbility(const SliteAbility *ability)
{
    nativeAbility_ = const_cast<SliteAbility *>(ability);
}

int32_t AbilityRecordManager::AddAbilityRecordObserver(AbilityRecordObserver *observer)
{
    AbilityRecordObserverManager::GetInstance().AddObserver(observer);
    return ERR_OK;
}

int32_t AbilityRecordManager::RemoveAbilityRecordObserver(AbilityRecordObserver *observer)
{
    AbilityRecordObserverManager::GetInstance().RemoveObserver(observer);
    return ERR_OK;
}

Want *AbilityRecordManager::CopyWant(const Want *want)
{
    if (want == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "want is nullptr");
        return nullptr;
    }
    Want *copiedWant = static_cast<Want *>(AdapterMalloc(sizeof(Want)));
    copiedWant->element = nullptr;
    copiedWant->data = OHOS::Utils::Memdup(want->data, want->dataLength);
    copiedWant->dataLength = want->dataLength;
    copiedWant->appPath = OHOS::Utils::Strdup(want->appPath);
    copiedWant->actions = nullptr;
    copiedWant->entities = nullptr;
    if (want->element != nullptr) {
        ElementName elementName = {};
        SetElementBundleName(&elementName, want->element->bundleName);
        SetWantElement(copiedWant, elementName);
        ClearElement(&elementName);
    }
    return copiedWant;
}

int32_t AbilityRecordManager::AddAbilityOperation(uint16_t msgId, const Want *want, uint64_t token)
{
    AbilityOperation *operation = static_cast<AbilityOperation *>(AdapterMalloc(sizeof(AbilityOperation)));
    if (operation == nullptr || memset_s(operation, sizeof(AbilityOperation), 0, sizeof(AbilityOperation)) != EOK) {
        AdapterFree(operation);
        HILOG_ERROR(HILOG_MODULE_AAFWK, "AddAbilityOperation failed");
        return PARAM_NULL_ERROR;
    }
    operation->msgId = msgId;
    operation->want = CopyWant(want);
    operation->token = token;
    abilityOperation_.PushBack(operation);
    return ERR_OK;
}

int32_t AbilityRecordManager::RunOperation()
{
    if (abilityOperation_.Size() == 0) {
        isAppScheduling_ = false;
        return ERR_OK;
    }

    if (isAppScheduling_) {
        return ERR_OK;
    }

    AbilityOperation *operation = abilityOperation_.Front();
    abilityOperation_.PopFront();
    if (operation == nullptr) {
        return PARAM_NULL_ERROR;
    }
    int32_t ret = ERR_OK;
    switch (operation->msgId) {
        case START_ABILITY: {
            ret = StartAbility(operation->want);
            break;
        }
        case TERMINATE_ABILITY: {
            ret = TerminateAbility(static_cast<uint16_t>(operation->token));
            break;
        }
        case TERMINATE_APP: {
            ret = ForceStopBundle(static_cast<uint16_t>(operation->token));
            break;
        }
        case TERMINATE_MISSION: {
            ret = TerminateMission(static_cast<uint32_t>(operation->token));
            break;
        }
        case TERMINATE_APP_BY_BUNDLENAME: {
            ret = ForceStop(operation->want);
            break;
        }
        case TERMINATE_ALL: {
            ret = TerminateAll(reinterpret_cast<char *>(operation->want->data));
            break;
        }
        default: {
            break;
        }
    }
    ClearWant(operation->want);
    AdapterFree(operation->want);
    AdapterFree(operation);
    if (ret != ERR_OK) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "RunOperation failed due to error : [%{public}d]", ret);
        isAppScheduling_ = false;
        return RunOperation();
    }

    if (!isAppScheduling_) {
        return RunOperation();
    }

    return ret;
}

void AbilityRecordManager::SetIsAppScheduling(bool runState)
{
    isAppScheduling_ = runState;
}

bool AbilityRecordManager::GetIsAppScheduling()
{
    return isAppScheduling_;
}
} // namespace AbilitySlite
} // namespace OHOS

extern "C" {
int InstallNativeAbility(const AbilityInfo *abilityInfo, const OHOS::AbilitySlite::SliteAbility *ability)
{
    OHOS::AbilitySlite::AbilityRecordManager::GetInstance().setNativeAbility(ability);
    return ERR_OK;
}

ElementName *GetTopAbility()
{
    return OHOS::AbilitySlite::AbilityRecordManager::GetInstance().GetTopAbility();
}
}
