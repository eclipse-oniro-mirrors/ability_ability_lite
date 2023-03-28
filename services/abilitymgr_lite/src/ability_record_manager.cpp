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
#include "ability_record.h"
#include "ability_stack.h"
#include "ability_state.h"
#include "abilityms_log.h"
#include "ability_manager_inner.h"
#include "bundle_manager.h"
#include "cmsis_os.h"
#ifdef OHOS_DMS_ENABLED
#include "dmsfwk_interface.h"
#endif
#include "js_app_host.h"
#include "los_task.h"
#ifdef OHOS_DMS_ENABLED
#include "samgr_lite.h"
#endif
#include "slite_ability.h"
#include "utils.h"
#include "want.h"

using namespace OHOS::ACELite;

namespace OHOS {
constexpr char LAUNCHER_BUNDLE_NAME[] = "com.ohos.launcher";
constexpr uint16_t LAUNCHER_TOKEN = 0;
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
    auto record = new AbilityRecord();
    record->SetAppName(LAUNCHER_BUNDLE_NAME);
    record->token = LAUNCHER_TOKEN;
    record->state = SCHEDULE_ACTIVE;
    record->taskId = LOS_CurTaskIDGet();
    abilityList_.Add(record);
    abilityStack_.PushAbility(record);
    (void) SchedulerLifecycleInner(record, STATE_ACTIVE);
}

void AbilityRecordManager::CleanWant()
{
    ClearWant(want_);
    AdapterFree(want_);
}

bool AbilityRecordManager::IsValidAbility(AbilityInfo *abilityInfo)
{
    if (abilityInfo == nullptr) {
        return false;
    }
    if (abilityInfo->bundleName == nullptr || abilityInfo->srcPath == nullptr) {
        return false;
    }
    if (strlen(abilityInfo->bundleName) == 0 || strlen(abilityInfo->srcPath) == 0) {
        return false;
    }
    return true;
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
    if (want == nullptr || want->element == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability Service wanted element is null");
        return PARAM_NULL_ERROR;
    }
    char *bundleName = want->element->bundleName;
    if (bundleName == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability Service wanted bundleName is null");
        return PARAM_NULL_ERROR;
    }

#ifdef OHOS_DMS_ENABLED
    if (want->element->deviceId != nullptr && *(want->element->deviceId) != '\0') {
        // deviceId is set
        return StartRemoteAbility(want);
    }
#endif

    AbilitySvcInfo *info =
        static_cast<OHOS::AbilitySvcInfo *>(AdapterMalloc(sizeof(AbilitySvcInfo)));
    if (info == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability Service AbilitySvcInfo is null");
        return PARAM_NULL_ERROR;
    }

    if (IsLauncher(bundleName)) {
        // Launcher
        info->bundleName = Utils::Strdup(bundleName);
        info->path = nullptr;
    } else {
        // JS APP
#if ((defined OHOS_APPEXECFWK_BMS_BUNDLEMANAGER) || (defined APP_PLATFORM_WATCHGT))
        AbilityInfo abilityInfo = { nullptr, nullptr };
        QueryAbilityInfo(want, &abilityInfo);
        if (!IsValidAbility(&abilityInfo)) {
            APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_START_UNKNOWN_BUNDLE_INFO);
            ClearAbilityInfo(&abilityInfo);
            AdapterFree(info);
            HILOG_ERROR(HILOG_MODULE_AAFWK, "Ability Service returned bundleInfo is not valid");
            return PARAM_NULL_ERROR;
        }
        info->bundleName = OHOS::Utils::Strdup(abilityInfo.bundleName);
        info->path = OHOS::Utils::Strdup(abilityInfo.srcPath);
        ClearAbilityInfo(&abilityInfo);
#else
        info->bundleName = Utils::Strdup(bundleName);
        // Here users assign want->data with js app path.
        info->path = Utils::Strdup((const char *)want->data);
#endif
    }

    info->data = OHOS::Utils::Memdup(want->data, want->dataLength);
    info->dataLength = want->dataLength;
    auto ret = StartAbility(info);
    AdapterFree(info->bundleName);
    AdapterFree(info->path);
    AdapterFree(info->data);
    AdapterFree(info);
    return ret;
}

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

int32_t AbilityRecordManager::StartAbility(AbilitySvcInfo *info)
{
    if ((info == nullptr) || (info->bundleName == nullptr) || (strlen(info->bundleName) == 0)) {
        return PARAM_NULL_ERROR;
    }
    HILOG_INFO(HILOG_MODULE_AAFWK, "StartAbility");

    auto topRecord = abilityStack_.GetTopAbility();
    if ((topRecord == nullptr) || (topRecord->appName == nullptr)) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "StartAbility top null.");
        return PARAM_NULL_ERROR;
    }
    uint16_t topToken = topRecord->token;
    //  start launcher
    if (IsLauncher(info->bundleName)) {
        UpdateRecord(info);
        if (topToken != LAUNCHER_TOKEN && topRecord->state != SCHEDULE_BACKGROUND) {
            HILOG_INFO(HILOG_MODULE_AAFWK, "Change Js app to background.");
            (void) SchedulerLifecycleInner(topRecord, STATE_BACKGROUND);
        } else {
            (void) SchedulerLifecycle(LAUNCHER_TOKEN, STATE_ACTIVE);
        }
        return ERR_OK;
    }

    if (!CheckResponse(info->bundleName)) {
        return PARAM_CHECK_ERROR;
    }

    // start js app
    if (topRecord->state != SCHEDULE_STOP && topRecord->token != LAUNCHER_TOKEN) {
        // start app is top
        if (strcmp(info->bundleName, topRecord->appName) == 0) {
            if (topRecord->state == SCHEDULE_BACKGROUND) {
                HILOG_INFO(HILOG_MODULE_AAFWK, "StartAbility Resume app when background.");
                (void) SchedulerLifecycle(LAUNCHER_TOKEN, STATE_BACKGROUND);
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
    return PreCheckStartAbility(info->bundleName, info->path, info->data, info->dataLength);
}

int32_t AbilityRecordManager::TerminateAbility(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "TerminateAbility [%{public}u]", token);
    AbilityRecord *topRecord = const_cast<AbilityRecord *>(abilityStack_.GetTopAbility());
    if (topRecord == nullptr) {
        APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_STOP_NO_ABILITY_RUNNING);
        return PARAM_NULL_ERROR;
    }
    uint16_t topToken = topRecord->token;
    if (token == LAUNCHER_TOKEN) {
        // if js is in background, the launcher goes back to background and js goes to active
        if (topToken != token && topRecord->state == SCHEDULE_BACKGROUND) {
            HILOG_INFO(HILOG_MODULE_AAFWK, "Resume Js app [%{public}u]", topToken);
            return SchedulerLifecycle(LAUNCHER_TOKEN, STATE_BACKGROUND);
        }
        return ERR_OK;
    }

    if (token != topToken) {
        APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_STOP_UNKNOWN_ABILITY_TOKEN);
        DeleteRecordInfo(token);
        return -1;
    }
    topRecord->isTerminated = true;
    // TerminateAbility top js
    return SchedulerLifecycleInner(topRecord, STATE_BACKGROUND);
}

int32_t AbilityRecordManager::ForceStopBundle(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "ForceStopBundle [%{public}u]", token);
    if (token == LAUNCHER_TOKEN) {
        HILOG_INFO(HILOG_MODULE_AAFWK, "Launcher does not support force stop.");
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
    if (launcherRecord->state != SCHEDULE_ACTIVE) {
        return SchedulerLifecycle(LAUNCHER_TOKEN, STATE_ACTIVE);
    }
    return ERR_OK;
}

int32_t AbilityRecordManager::ForceStop(const char *bundleName)
{
    if (bundleName == nullptr) {
        return PARAM_NULL_ERROR;
    }

    // stop Launcher
    if (IsLauncher(bundleName)) {
        return TerminateAbility(0);
    }

    // stop js app
    if (strcmp(abilityStack_.GetTopAbility()->appName, bundleName) == 0) {
        AbilityRecord *topRecord = const_cast<AbilityRecord *>(abilityStack_.GetTopAbility());
        HILOG_INFO(HILOG_MODULE_AAFWK, "ForceStop [%{public}u]", topRecord->token);
        return TerminateAbility(topRecord->token);
    }
    return PARAM_CHECK_ERROR;
}

int32_t AbilityRecordManager::ForceStopBundleInner(uint16_t token)
{
    // free js mem and delete the record
    AbilityRecord *record = abilityList_.Get(token);
    if (record == nullptr) {
        return PARAM_NULL_ERROR;
    }
    auto jsAppHost = const_cast<JsAppHost *>(record->jsAppHost);
    if (jsAppHost != nullptr) {
        // free js mem
        jsAppHost->ForceDestroy();
    }
    DeleteRecordInfo(token);
    return ERR_OK;
}

int32_t AbilityRecordManager::PreCheckStartAbility(
    const char *bundleName, const char *path, const void *data, uint16_t dataLength)
{
    if (path == nullptr) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "PreCheckStartAbility path is null.");
        return PARAM_NULL_ERROR;
    }
    auto curRecord = abilityList_.Get(bundleName);
    if (curRecord != nullptr) {
        if (curRecord->state == SCHEDULE_ACTIVE) {
            HILOG_ERROR(HILOG_MODULE_AAFWK, "PreCheckStartAbility current state active.");
        } else if (curRecord->state == SCHEDULE_BACKGROUND) {
            SchedulerLifecycle(LAUNCHER_TOKEN, STATE_BACKGROUND);
        }
        return ERR_OK;
    }
    auto record = new AbilityRecord();
    if (pendingToken_ != 0) {
        record->token = pendingToken_;
    } else {
        record->token = GenerateToken();
    }
    record->SetAppName(bundleName);
    record->SetAppPath(path);
    record->SetWantData(data, dataLength);
    record->state = SCHEDULE_STOP;
    abilityList_.Add(record);
    if (pendingToken_ == 0 && CreateAppTask(record) != ERR_OK) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "CheckResponse CreateAppTask fail");
        abilityList_.Erase(record->token);
        delete record;
        return CREATE_APPTASK_ERROR;
    }
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

    HILOG_INFO(HILOG_MODULE_AAFWK, "CreateAppTask.");
    TSK_INIT_PARAM_S stTskInitParam = { 0 };
    LOS_TaskLock();
    stTskInitParam.pfnTaskEntry = (TSK_ENTRY_FUNC) (JsAppHost::JsAppTaskHandler);
    stTskInitParam.uwStackSize = TASK_STACK_SIZE;
    stTskInitParam.usTaskPrio = OS_TASK_PRIORITY_LOWEST - APP_TASK_PRI;
    stTskInitParam.pcName = const_cast<char *>("AppTask");
    stTskInitParam.uwResved = 0;
    auto jsAppHost = new JsAppHost();
    stTskInitParam.uwArg = reinterpret_cast<UINT32>((uintptr_t) jsAppHost);
    UINT32 appTaskId = 0;
    UINT32 ret = LOS_TaskCreate(&appTaskId, &stTskInitParam);
    if (ret != LOS_OK) {
        HILOG_ERROR(HILOG_MODULE_AAFWK, "CreateAppTask fail: ret = %{public}d", ret);
        APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_START_CREATE_TSAK_FAILED);
        delete jsAppHost;
        LOS_TaskUnlock();
        return CREATE_APPTASK_ERROR;
    }
    osMessageQueueId_t jsAppQueueId = osMessageQueueNew(QUEUE_LENGTH, sizeof(AbilityInnerMsg), nullptr);
    jsAppHost->SetMessageQueueId(jsAppQueueId);
    LOS_TaskUnlock();

    record->taskId = appTaskId;
    record->jsAppQueueId = jsAppQueueId;
    record->jsAppHost = jsAppHost;
    record->state = SCHEDULE_INACTIVE;
    abilityStack_.PushAbility(record);
    APP_EVENT(MT_ACE_APP_START);
    if (nativeAbility_ != nullptr) {
        if (SchedulerLifecycle(LAUNCHER_TOKEN, STATE_BACKGROUND) != 0) {
            APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_START_LAUNCHER_EXIT_FAILED);
            HILOG_INFO(HILOG_MODULE_AAFWK, "CreateAppTask Fail to hide launcher");
            abilityStack_.PopAbility();
            return SCHEDULER_LIFECYCLE_ERROR;
        }
    } else {
        SchedulerLifecycle(record->token, STATE_ACTIVE);
    }
    return ERR_OK;
}

uint16_t AbilityRecordManager::GenerateToken()
{
    static uint16_t token = LAUNCHER_TOKEN;
    if (token == UINT16_MAX - 1) {
        token = LAUNCHER_TOKEN;
    }
    return ++token;
}

void AbilityRecordManager::DeleteRecordInfo(uint16_t token)
{
    AbilityRecord *record = abilityList_.Get(token);
    if (record == nullptr) {
        return;
    }
    if (token != LAUNCHER_TOKEN) {
        if (record->state != SCHEDULE_STOP) {
            UINT32 taskId = record->taskId;
            // LiteOS-M not support permissions checking right now, when permission checking is
            // ready, we can remove the macro.
            LOS_TaskDelete(taskId);
            osMessageQueueId_t jsAppQueueId = record->jsAppQueueId;
            osMessageQueueDelete(jsAppQueueId);
            auto jsAppHost = const_cast<JsAppHost *>(record->jsAppHost);
            delete jsAppHost;
            // free all JS native memory after exiting it
            // CleanTaskMem(taskId)
        }
        // record app info event when stop app
        RecordAbiityInfoEvt(record->GetAppName());
    }
    abilityStack_.Erase(record);
    abilityList_.Erase(token);
    delete record;
}

void AbilityRecordManager::OnActiveDone(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "OnActiveDone [%{public}u]", token);
    SetAbilityState(token, SCHEDULE_ACTIVE);
    auto topRecord = const_cast<AbilityRecord *>(abilityStack_.GetTopAbility());
    if (topRecord == nullptr) {
        return;
    }

    // the launcher active
    if (token == LAUNCHER_TOKEN) {
        if (nativeAbility_ == nullptr || nativeAbility_->GetState() != STATE_ACTIVE) {
            HILOG_ERROR(HILOG_MODULE_AAFWK, "native ability is in wrong state : %{public}d",
                nativeAbility_->GetState());
            return;
        }
        if (topRecord->token != LAUNCHER_TOKEN) {
            int abilityState = STATE_UNINITIALIZED;
            if (topRecord->state == SCHEDULE_ACTIVE) {
                HILOG_ERROR(HILOG_MODULE_AAFWK,
                    "js is in active state, native state is %{public}d", abilityState);
                OnDestroyDone(topRecord->token);
                return;
            }
            if (topRecord->state != SCHEDULE_BACKGROUND) {
                APP_ERRCODE_EXTRA(EXCE_ACE_APP_START, EXCE_ACE_APP_START_LAUNCHER_EXIT_FAILED);
                HILOG_ERROR(HILOG_MODULE_AAFWK,
                    "Active launcher js bg fail, native state is %{public}d", abilityState);
                abilityStack_.PopAbility();
                DeleteRecordInfo(topRecord->token);
            } else if (topRecord->isTerminated) {
                (void) SchedulerLifecycleInner(topRecord, STATE_UNINITIALIZED);
            }
        }
        return;
    }
    // the js app active
    if (topRecord->token == token) {
        APP_EVENT(MT_ACE_APP_ACTIVE);
    }
}

void AbilityRecordManager::OnBackgroundDone(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "OnBackgroundDone [%{public}u]", token);
    SetAbilityState(token, SCHEDULE_BACKGROUND);
    auto topRecord = const_cast<AbilityRecord *>(abilityStack_.GetTopAbility());
    if (topRecord == nullptr) {
        return;
    }
    // the js background
    if (token != LAUNCHER_TOKEN) {
        if (topRecord->token == token) {
            APP_EVENT(MT_ACE_APP_BACKGROUND);
            (void) SchedulerLifecycle(LAUNCHER_TOKEN, STATE_ACTIVE);
        }
        return;
    }
    // the launcher background
    if (topRecord->token != LAUNCHER_TOKEN) {
        (void) SchedulerLifecycleInner(topRecord, STATE_ACTIVE);
        if (GetCleanAbilityDataFlag()) {
            HILOG_INFO(HILOG_MODULE_AAFWK, "OnBackgroundDone clean launcher record data");
            AbilityRecord *record = abilityList_.Get(token);
            record->SetWantData(nullptr, 0);
            SetCleanAbilityDataFlag(false);
        }
        return;
    }
    HILOG_WARN(HILOG_MODULE_AAFWK, "Js app exit, but has no js app.");
}

void AbilityRecordManager::OnDestroyDone(uint16_t token)
{
    HILOG_INFO(HILOG_MODULE_AAFWK, "OnDestroyDone [%{public}u]", token);
    // the launcher destroy
    if (token == LAUNCHER_TOKEN) {
        SetAbilityState(token, SCHEDULE_STOP);
        return;
    }
    auto topRecord = abilityStack_.GetTopAbility();
    if ((topRecord == nullptr) || (topRecord->token != token)) {
        SetAbilityState(token, SCHEDULE_STOP);
        DeleteRecordInfo(token);
        return;
    }
    APP_EVENT(MT_ACE_APP_STOP);
    abilityStack_.PopAbility();
    DeleteRecordInfo(token);
    SetAbilityState(token, SCHEDULE_STOP);

    // no pending token
    if (pendingToken_ == 0) {
        (void) SchedulerLifecycle(LAUNCHER_TOKEN, STATE_ACTIVE);
        return;
    }

    // start pending token
    auto record = abilityList_.Get(pendingToken_);
    if (CreateAppTask(record) != ERR_OK) {
        abilityList_.Erase(pendingToken_);
        delete record;
        (void) SchedulerLifecycle(LAUNCHER_TOKEN, STATE_ACTIVE);
    }
    pendingToken_ = 0;
}

int32_t AbilityRecordManager::SchedulerLifecycle(uint64_t token, int32_t state)
{
    AbilityRecord *record = abilityList_.Get(token);
    if (record == nullptr) {
        return PARAM_NULL_ERROR;
    }
    return SchedulerLifecycleInner(record, state);
}

void AbilityRecordManager::SetAbilityState(uint64_t token, int32_t state)
{
    AbilityRecord *record = abilityList_.Get(token);
    if (record == nullptr) {
        return;
    }
    record->state = state;
}

int32_t AbilityRecordManager::SchedulerLifecycleInner(const AbilityRecord *record, int32_t state)
{
    if (record == nullptr) {
        return PARAM_NULL_ERROR;
    }
    // dispatch js life cycle
    if (record->token != LAUNCHER_TOKEN) {
        (void) SendMsgToJsAbility(state, record);
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

void AbilityRecordManager::SchedulerAbilityLifecycle(SliteAbility *ability, const Want &want, int32_t state)
{
    if (ability == nullptr) {
        return;
    }
    switch (state) {
        case STATE_ACTIVE: {
            ability->OnActive(want);
            break;
        }
        case STATE_BACKGROUND: {
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
        case STATE_ACTIVE: {
            OnActiveDone(token);
            break;
        }
        case STATE_BACKGROUND: {
            OnBackgroundDone(token);
            break;
        }
        case STATE_UNINITIALIZED: {
            OnDestroyDone(token);
            break;
        }
        default: {
            break;
        }
    }
    return ERR_OK;
}

bool AbilityRecordManager::SendMsgToJsAbility(int32_t state, const AbilityRecord *record)
{
    if (record == nullptr) {
        return false;
    }

    AbilityInnerMsg innerMsg;
    if (state == STATE_ACTIVE) {
        innerMsg.msgId = ACTIVE;
    } else if (state == STATE_BACKGROUND) {
        innerMsg.msgId = BACKGROUND;
    } else if (state == STATE_UNINITIALIZED) {
        innerMsg.msgId = DESTROY;
    } else {
        innerMsg.msgId = (AbilityMsgId) state;
    }
    innerMsg.bundleName = record->appName;
    innerMsg.token = record->token;
    innerMsg.path = record->appPath;
    if (record->abilityData != nullptr) {
        innerMsg.data = const_cast<void *>(record->abilityData->wantData);
        innerMsg.dataLength = record->abilityData->wantDataSize;
    } else {
        innerMsg.data = nullptr;
        innerMsg.dataLength = 0;
    }
    osMessageQueueId_t appQueueId = record->jsAppQueueId;
    osStatus_t ret = osMessageQueuePut(appQueueId, static_cast<void *>(&innerMsg), 0, 0);
    return ret == osOK;
}

ElementName *AbilityRecordManager::GetTopAbility()
{
    auto topRecord = const_cast<AbilityRecord *>(abilityStack_.GetTopAbility());
    AbilityRecord *launcherRecord = abilityList_.Get(LAUNCHER_TOKEN);
    if (topRecord == nullptr || launcherRecord == nullptr) {
        return nullptr;
    }
    ElementName *element = reinterpret_cast<ElementName *>(AdapterMalloc(sizeof(ElementName)));
    if (element == nullptr || memset_s(element, sizeof(ElementName), 0, sizeof(ElementName)) != EOK) {
        AdapterFree(element);
        return nullptr;
    }
    if (topRecord->token == LAUNCHER_TOKEN || launcherRecord->state == SCHEDULE_ACTIVE) {
        SetElementBundleName(element, LAUNCHER_BUNDLE_NAME);
        return element;
    }

    // case js active or background when launcher not active
    if (topRecord->state == SCHEDULE_ACTIVE || topRecord->state == SCHEDULE_BACKGROUND) {
        SetElementBundleName(element, topRecord->appName);
    }
    return element;
}

void AbilityRecordManager::setNativeAbility(const SliteAbility *ability)
{
    nativeAbility_ = const_cast<SliteAbility *>(ability);
}
} // namespace OHOS

extern "C" {
int InstallNativeAbility(const AbilityInfo *abilityInfo, const OHOS::SliteAbility *ability)
{
    OHOS::AbilityRecordManager::GetInstance().setNativeAbility(ability);
    return ERR_OK;
}

ElementName *GetTopAbility()
{
    return OHOS::AbilityRecordManager::GetInstance().GetTopAbility();
}
}