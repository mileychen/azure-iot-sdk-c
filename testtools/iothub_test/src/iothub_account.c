// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifdef __cplusplus
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#else
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#endif

#include "azure_c_shared_utility/string_tokenizer.h"
#include "iothub_account.h"

#include "azure_c_shared_utility/httpapiexsas.h"
#include "azure_c_shared_utility/base64.h"
#include "azure_c_shared_utility/hmacsha256.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/sastoken.h"

#include "azure_c_shared_utility/httpapiexsas.h"
#include "azure_c_shared_utility/base64.h"
#include "azure_c_shared_utility/uniqueid.h"

#include "iothub_service_client_auth.h"
#include "iothub_registrymanager.h"

#ifdef MBED_BUILD_TIMESTAMP
#define MBED_PARAM_MAX_LENGTH 256
#endif

static const char* URL_API_VERSION = "api-version=2016-11-14";
static const char* DEVICE_JSON_FMT = "{\"deviceId\":\"%s\",\"etag\":null,\"connectionState\":\"Disconnected\",\"status\":\"enabled\",\"statusReason\":null,\"connectionStateUpdatedTime\":\"0001-01-01T00:00:00\",\"statusUpdatedTime\":\"0001-01-01T00:00:00\",\"lastActivityTime\":\"0001-01-01T00:00:00\",\"authentication\":{\"symmetricKey\":{\"primaryKey\":null,\"secondaryKey\":null}}}";
static const char* SAS_DEVICE_PREFIX_FMT = "csdk_e2eDevice_sas_j_please_delete_%s";
static const char* X509_DEVICE_PREFIX_FMT = "csdk_e2eDevice_x509_j_please_delete_%s";
static const char* RELATIVE_PATH_FMT = "/devices/%s?%s";
static const char* SHARED_ACCESS_KEY = "SharedAccessSignature sr=%s&sig=%s&se=%s&skn=%s";

static const char *DEFAULT_CONSUMER_GROUP = "$Default";
static const int DEFAULT_PARTITION_COUNT = 16;

static const char* PRIMARY_KEY_FIELD = "\"primaryKey\":\"";
static const size_t PRIMARY_KEY_FIELD_LEN = 14;


#define MAX_LENGTH_OF_UNSIGNED_INT  6
#define MAX_LENGTH_OF_TIME_VALUE    12
#define MAX_RAND_VALUE              10000
#define DEVICE_GUID_SIZE            37


typedef struct IOTHUB_ACCOUNT_INFO_TAG
{
    const char* connString;
    const char* eventhubConnString;
    char* hostname;
    char* iothubName;
    char* iothubSuffix;
    char* sharedAccessKey;
    char* sharedAccessToken;
    char* keyName;
    char* eventhubAccessKey;
    char* x509Certificate;
    char* x509PrivateKey;
    char* x509Thumbprint;
    IOTHUB_PROVISIONED_DEVICE sasDevice;
    IOTHUB_PROVISIONED_DEVICE x509Device;
    IOTHUB_SERVICE_CLIENT_AUTH_HANDLE iothub_service_client_auth_handle;
    IOTHUB_REGISTRYMANAGER_HANDLE iothub_registrymanager_handle;
    IOTHUB_MESSAGING_HANDLE iothub_messaging_handle;
} IOTHUB_ACCOUNT_INFO;

static HTTP_HEADERS_HANDLE getContentHeaders(bool appendIfMatch)
{
    HTTP_HEADERS_HANDLE httpHeader = HTTPHeaders_Alloc();
    if (httpHeader != NULL)
    {
        if (HTTPHeaders_AddHeaderNameValuePair(httpHeader, "Authorization", " ") != HTTP_HEADERS_OK ||
            HTTPHeaders_AddHeaderNameValuePair(httpHeader, "User-Agent", "Microsoft.Azure.Devices/1.0.0") != HTTP_HEADERS_OK ||
            HTTPHeaders_AddHeaderNameValuePair(httpHeader, "Accept", "application/json") != HTTP_HEADERS_OK ||
            HTTPHeaders_AddHeaderNameValuePair(httpHeader, "Content-Type", "application/json; charset=utf-8") != HTTP_HEADERS_OK)
        {
            LogError("Failure adding http headers.\r\n");
            HTTPHeaders_Free(httpHeader);
            httpHeader = NULL;
        }
        else
        {
            if (appendIfMatch)
            {
                if (HTTPHeaders_AddHeaderNameValuePair(httpHeader, "If-Match", "*") != HTTP_HEADERS_OK)
                {
                    LogError("Failure adding if-Match http headers.\r\n");
                    HTTPHeaders_Free(httpHeader);
                    httpHeader = NULL;
                }
            }
        }
    }
    return httpHeader;
}

static int generateDeviceName(const char* prefix, char** deviceName)
{
    int result;
    char deviceGuid[DEVICE_GUID_SIZE];
    if (UniqueId_Generate(deviceGuid, DEVICE_GUID_SIZE) != UNIQUEID_OK)
    {
        LogError("Unable to generate unique Id.\r\n");
        result = __LINE__;
    }
    else
    {
        size_t len = strlen(prefix) + DEVICE_GUID_SIZE;
        *deviceName = (char*)malloc(len + 1);
        if (*deviceName == NULL)
        {
            LogError("Failure allocating device ID.\r\n");
            result = __LINE__;
        }
        else
        {
            if (sprintf_s(*deviceName, len + 1, prefix, deviceGuid) <= 0)
            {
                LogError("Failure constructing device ID.\r\n");
                free(*deviceName);
                result = __LINE__;
            }
            else
            {
                LogInfo("Created Device %s.", *deviceName);
                result = 0;
            }
        }
    }
    return result;
}

static int retrieveConnStringInfo(IOTHUB_ACCOUNT_INFO* accountInfo)
{
    int result;
    int beginName, endName, beginIothub, endIothub, beginHost, endHost, beginKey;
    size_t totalLen = strlen(accountInfo->connString);

    if (sscanf(accountInfo->connString, "HostName=%n%*[^.]%n.%n%*[^;];%nSharedAccessKeyName=%n%*[^;];%nSharedAccessKey=%n", &beginHost, &endHost, &beginIothub, &endIothub, &beginName, &endName, &beginKey) != 0)
    {
        LogError("Failure determining the string length parameters.\r\n");
        result = __LINE__;
    }
    else
    {
        if ((accountInfo->iothubName = (char*)malloc(endHost - beginHost + 1)) == NULL)
        {
            LogError("Failure allocating iothubName.\r\n");
            result = __LINE__;
        }
        else if ((accountInfo->hostname = (char*)malloc(endIothub - beginHost + 1)) == NULL)
        {
            LogError("Failure allocating hostname.\r\n");
            free(accountInfo->iothubName);
            result = __LINE__;
        }
        else if ((accountInfo->keyName = (char*)malloc(endName - beginName + 1)) == NULL)
        {
            LogError("Failure allocating hostName.\r\n");
            free(accountInfo->iothubName);
            free(accountInfo->hostname);
            result = __LINE__;
        }
        else if ((accountInfo->sharedAccessKey = (char*)malloc(totalLen + 1 - beginKey + 1)) == NULL)
        {
            LogError("Failure allocating hostName.\r\n");
            free(accountInfo->iothubName);
            free(accountInfo->keyName);
            free(accountInfo->hostname);
            result = __LINE__;
        }
        else if (sscanf(accountInfo->connString, "HostName=%[^.].%[^;];SharedAccessKeyName=%[^;];SharedAccessKey=%s", accountInfo->iothubName,
            accountInfo->hostname + endHost - beginHost + 1,
            accountInfo->keyName,
            accountInfo->sharedAccessKey) != 4)
        {
            LogError("Failure determining the string values.\r\n");
            free(accountInfo->iothubName);
            free(accountInfo->hostname);
            free(accountInfo->keyName);
            free(accountInfo->sharedAccessKey);
            result = __LINE__;
        }
        else
        {
            (void)strcpy(accountInfo->hostname, accountInfo->iothubName);
            accountInfo->hostname[endHost - beginHost] = '.';
            if (mallocAndStrcpy_s(&accountInfo->iothubSuffix, accountInfo->hostname + endHost - beginHost + 1) != 0)
            {
                LogError("[IoTHubAccount] Failure constructing the iothubSuffix.");
                free(accountInfo->iothubName);
                free(accountInfo->hostname);
                free(accountInfo->keyName);
                free(accountInfo->sharedAccessKey);
                result = __LINE__;
            }
            else
            {
                result = 0;
            }
        }
    }
    return result;
}

#ifdef MBED_BUILD_TIMESTAMP
static const char* getMbedParameter(const char* name)
{
    static char value[MBED_PARAM_MAX_LENGTH];
    (void)printf("%s?\r\n", name);
    (void)scanf("%s", &value);
    (void)printf("Received '%s'\r\n", value);				

    return value;	
}
#endif
#define CONN_HOST_PART "HostName="
#define CONN_DEVICE_PART ";DeviceId="
#define CONN_KEY_PART ";SharedAccessKey="
#define CONN_X509_PART ";x509=true"

static int createSASConnectionString(IOTHUB_ACCOUNT_INFO* accountInfo, char** connectionString) {

    int result;
    char* conn;
    size_t sizeOfHostPart = strlen(CONN_HOST_PART);

    size_t sizeOfDevicePart = strlen(CONN_DEVICE_PART);

    size_t sizeOfKeyPart = strlen(CONN_KEY_PART);

    size_t sizeOfHostName = strlen(accountInfo->hostname);
    size_t sizeOfDeviceId = strlen(accountInfo->sasDevice.deviceId);
    size_t sizeOfDeviceKey = strlen(accountInfo->sasDevice.primaryAuthentication);

    size_t connectionStringLength = sizeOfHostPart + sizeOfDevicePart + sizeOfKeyPart + sizeOfHostName + sizeOfDeviceId + sizeOfDeviceKey + 1;

    conn = (char*)malloc(connectionStringLength);
    if (conn == NULL) {
        LogError("Failed to allocate space for the SAS based connection string\r\n");
        result = __LINE__;
    }
    else if (sprintf_s(conn, connectionStringLength,"%s%s%s%s%s%s", CONN_HOST_PART, accountInfo->hostname, CONN_DEVICE_PART, (char*)accountInfo->sasDevice.deviceId, CONN_KEY_PART, (char*)accountInfo->sasDevice.primaryAuthentication) <= 0) {
            LogError("Failed to form the connection string for SAS based connection string.\r\n");
            result = __LINE__;
            free(conn);
    }
    else
    {
        *connectionString = conn;
        result = 0;
    }
    return result;

}

static int createX509ConnectionString(IOTHUB_ACCOUNT_INFO* accountInfo, char** connectionString) {

    int result;
    char* conn;

    size_t sizeOfHostPart = strlen(CONN_HOST_PART);
    size_t sizeOfDevicePart = strlen(CONN_DEVICE_PART);
    size_t sizeOfX509Part = strlen(CONN_X509_PART);

    size_t sizeOfHostName = strlen(accountInfo->hostname);
    size_t sizeOfDeviceId = strlen(accountInfo->x509Device.deviceId);

    size_t connectionStringLength = sizeOfHostPart + sizeOfDevicePart + sizeOfX509Part + sizeOfHostName + sizeOfDeviceId + 1;

    conn = (char*)malloc(connectionStringLength);
    if (conn == NULL) {
        LogError("Failed to allocate space for the SAS based connection string\r\n");
        result = __LINE__;
    }
    else if (sprintf_s(conn, connectionStringLength, "%s%s%s%s%s", CONN_HOST_PART, accountInfo->hostname, CONN_DEVICE_PART, (char*)accountInfo->x509Device.deviceId, CONN_X509_PART) <=0) {
        LogError("Failed to form the connection string for x509 based connection string.\r\n");
        result = __LINE__;
        free(conn);
    }
    else
    {
        *connectionString = conn;
        result = 0;
    }
    return result;
}

static int provisionDevice(IOTHUB_ACCOUNT_INFO* accountInfo, IOTHUB_ACCOUNT_AUTH_METTHOD method) {

    int result;
    char* deviceId = NULL;

    if (generateDeviceName(method == IOTHUB_ACCOUNT_AUTH_CONNSTRING ?(SAS_DEVICE_PREFIX_FMT):(X509_DEVICE_PREFIX_FMT), &deviceId) != 0)
    {
        LogError("generateDeviceName failed\r\n");
        result = __LINE__;
    }
    else
    {

        IOTHUB_PROVISIONED_DEVICE* deviceToProvision;
        IOTHUB_REGISTRYMANAGER_RESULT iothub_registrymanager_result;
        IOTHUB_REGISTRY_DEVICE_CREATE deviceCreateInfo;
        IOTHUB_DEVICE deviceInfo;
        deviceInfo.deviceId = NULL;
        deviceInfo.primaryKey = NULL;
        deviceInfo.secondaryKey = NULL;
        deviceInfo.generationId = NULL;
        deviceInfo.eTag = NULL;
        deviceInfo.connectionStateUpdatedTime = NULL;
        deviceInfo.statusReason = NULL;
        deviceInfo.statusUpdatedTime = NULL;
        deviceInfo.lastActivityTime = NULL;
        deviceInfo.configuration = NULL;
        deviceInfo.deviceProperties = NULL;
        deviceInfo.serviceProperties = NULL;

        deviceCreateInfo.deviceId = deviceId;
        if (method == IOTHUB_ACCOUNT_AUTH_CONNSTRING)
        {
            deviceToProvision = &accountInfo->sasDevice;
            deviceToProvision->howToCreate = IOTHUB_ACCOUNT_AUTH_CONNSTRING;
            deviceCreateInfo.primaryKey = "";
            deviceCreateInfo.secondaryKey = "";
            deviceCreateInfo.authMethod = IOTHUB_REGISTRYMANAGER_AUTH_SPK;
        }
        else
        {
            deviceToProvision = &accountInfo->x509Device;
            deviceToProvision->howToCreate = IOTHUB_ACCOUNT_AUTH_X509;
            deviceCreateInfo.primaryKey = accountInfo->x509Thumbprint;
            deviceCreateInfo.secondaryKey = "";
            deviceCreateInfo.authMethod = IOTHUB_REGISTRYMANAGER_AUTH_X509_THUMBPRINT;
        }

        iothub_registrymanager_result = IoTHubRegistryManager_CreateDevice(accountInfo->iothub_registrymanager_handle, &deviceCreateInfo, &deviceInfo);
        if (iothub_registrymanager_result != IOTHUB_REGISTRYMANAGER_OK)
        {
            LogError("IoTHubRegistryManager_CreateDevice failed\r\n");
            free(deviceId);
            result = __LINE__;
        }
        else
        {
            deviceToProvision->deviceId = deviceId;
            if (method == IOTHUB_ACCOUNT_AUTH_CONNSTRING)
            {
                if (mallocAndStrcpy_s((char**)&deviceToProvision->primaryAuthentication, (char*)deviceInfo.primaryKey) != 0)
                {
                    LogError("mallocAndStrcpy_s failed for primaryKey\r\n");
                    result = __LINE__;

                }
                else
                {
                    if (createSASConnectionString(accountInfo, &deviceToProvision->connectionString) != 0)
                    {
                        result = __LINE__;
                    }
                    else
                    {
                        result = 0;
                    }
                }
            }
            else
            {
                if (createX509ConnectionString(accountInfo, &deviceToProvision->connectionString) != 0)
                {
                    result = __LINE__;
                }
                else
                {
                    deviceToProvision->certificate = accountInfo->x509Certificate;
                    deviceToProvision->primaryAuthentication = accountInfo->x509PrivateKey;
                    result = 0;
                }
            }
        }

        if (deviceInfo.deviceId != NULL)
            free((char*)deviceInfo.deviceId);
        if (deviceInfo.primaryKey != NULL)
            free((char*)deviceInfo.primaryKey);
        if (deviceInfo.secondaryKey != NULL)
            free((char*)deviceInfo.secondaryKey);
        if (deviceInfo.generationId != NULL)
            free((char*)deviceInfo.generationId);
        if (deviceInfo.eTag != NULL)
            free((char*)deviceInfo.eTag);
        if (deviceInfo.connectionStateUpdatedTime != NULL)
            free((char*)deviceInfo.connectionStateUpdatedTime);
        if (deviceInfo.statusReason != NULL)
            free((char*)deviceInfo.statusReason);
        if (deviceInfo.statusUpdatedTime != NULL)
            free((char*)deviceInfo.statusUpdatedTime);
        if (deviceInfo.lastActivityTime != NULL)
            free((char*)deviceInfo.lastActivityTime);
        if (deviceInfo.configuration != NULL)
            free((char*)deviceInfo.configuration);
        if (deviceInfo.deviceProperties != NULL)
            free((char*)deviceInfo.deviceProperties);
        if (deviceInfo.serviceProperties != NULL)
            free((char*)deviceInfo.serviceProperties);
    }

    return result;
}


IOTHUB_ACCOUNT_INFO_HANDLE IoTHubAccount_Init(void)
{
    IOTHUB_ACCOUNT_INFO* iothub_account_info = malloc(sizeof(IOTHUB_ACCOUNT_INFO));
    if (iothub_account_info == NULL)
    {
        LogError("[IoTHubAccount] Failed allocating IOTHUB_ACCOUNT_INFO.");
    }
    else
    {
        memset(iothub_account_info, 0, sizeof(IOTHUB_ACCOUNT_INFO));
        
#ifdef MBED_BUILD_TIMESTAMP
        iothub_account_info->connString = getMbedParameter("IOTHUB_CONNECTION_STRING");
        iothub_account_info->eventhubConnString = getMbedParameter("IOTHUB_EVENTHUB_CONNECTION_STRING");
        iothub_account_info->x509Certificate = getMbedParameter("IOTHUB_E2E_X509_CERT");
        iothub_account_info->x509PrivateKey = getMbedParameter("IOTHUB_E2E_X509_PRIVATE_KEY");
        iothub_account_info->x509Thumbprint = getMbedParameter("IOTHUB_E2E_X509_THUMBPRINT");
#else
        iothub_account_info->connString = getenv("IOTHUB_CONNECTION_STRING");
        iothub_account_info->eventhubConnString = getenv("IOTHUB_EVENTHUB_CONNECTION_STRING");
        iothub_account_info->x509Certificate = getenv("IOTHUB_E2E_X509_CERT");
        iothub_account_info->x509PrivateKey = getenv("IOTHUB_E2E_X509_PRIVATE_KEY");
        iothub_account_info->x509Thumbprint = getenv("IOTHUB_E2E_X509_THUMBPRINT");
#endif
        
        if (iothub_account_info->connString == NULL)
        {
            LogError("Failure retrieving IoT Hub connection string from the environment.\r\n");
            free(iothub_account_info);
            iothub_account_info = NULL;
        }
        else if (iothub_account_info->eventhubConnString == NULL)
        {
            LogError("Failure retrieving Event Hub connection string from the environment.\r\n");
            free(iothub_account_info);
            iothub_account_info = NULL;
        }
        else if (iothub_account_info->x509Certificate == NULL)
        {
            LogError("Failure retrieving x509 certificate from the environment.\r\n");
            free(iothub_account_info);
            iothub_account_info = NULL;
        }
        else if (iothub_account_info->x509PrivateKey == NULL)
        {
            LogError("Failure retrieving x509 private key from the environment.\r\n");
            free(iothub_account_info);
            iothub_account_info = NULL;
        }
        else if (iothub_account_info->x509Thumbprint == NULL)
        {
            LogError("Failure retrieving x509 certificate thumbprint from the environment.\r\n");
            free(iothub_account_info);
            iothub_account_info = NULL;
        }
        else
        {
            if (retrieveConnStringInfo(iothub_account_info) != 0)
            {
                LogError("retrieveConnStringInfo failed.\r\n");
                free(iothub_account_info);
                iothub_account_info = NULL;
            }
            else
            {
                iothub_account_info->iothub_service_client_auth_handle = IoTHubServiceClientAuth_CreateFromConnectionString(iothub_account_info->connString);
                if (iothub_account_info->iothub_service_client_auth_handle == NULL)
                {
                    LogError("IoTHubServiceClientAuth_CreateFromConnectionString failed\r\n");
                    free(iothub_account_info);
                    iothub_account_info = NULL;
                }
                else
                {
                    iothub_account_info->iothub_messaging_handle = IoTHubMessaging_LL_Create(iothub_account_info->iothub_service_client_auth_handle);
                    if (iothub_account_info->iothub_messaging_handle == NULL)
                    {
                        LogError("IoTHubMessaging_LL_Create failed\r\n");
                        IoTHubAccount_deinit(iothub_account_info);
                        iothub_account_info = NULL;
                    }
                    else
                    {
                        iothub_account_info->iothub_registrymanager_handle = IoTHubRegistryManager_Create(iothub_account_info->iothub_service_client_auth_handle);
                        if (iothub_account_info->iothub_registrymanager_handle == NULL)
                        {
                            LogError("IoTHubRegistryManager_Create failed\r\n");
                            IoTHubAccount_deinit(iothub_account_info);
                            iothub_account_info = NULL;
                        }
                        else
                        {
                            if (provisionDevice(iothub_account_info, IOTHUB_ACCOUNT_AUTH_CONNSTRING) != 0)
                            {
                                LogError("Failed to create the SAS device\r\n");
                                IoTHubAccount_deinit(iothub_account_info);
                                iothub_account_info = NULL;
                            }
                            else
                            {
                                if (provisionDevice(iothub_account_info, IOTHUB_ACCOUNT_AUTH_X509) != 0)
                                {
                                    LogError("Failed to create the SAS device\r\n");
                                    IoTHubAccount_deinit(iothub_account_info);
                                    iothub_account_info = NULL;
                                }
                            }

                        }
                    }
                }
            }
        }
    }
    return (IOTHUB_ACCOUNT_INFO_HANDLE)iothub_account_info;
}

void IoTHubAccount_deinit(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    if (acctHandle != NULL)
    {
        IOTHUB_ACCOUNT_INFO* acctInfo = (IOTHUB_ACCOUNT_INFO*)acctHandle;

        IOTHUB_REGISTRYMANAGER_RESULT iothub_registrymanager_result;
        if (acctInfo->sasDevice.deviceId)
        {
            iothub_registrymanager_result = IoTHubRegistryManager_DeleteDevice(acctInfo->iothub_registrymanager_handle, acctInfo->sasDevice.deviceId);
            if (iothub_registrymanager_result != IOTHUB_REGISTRYMANAGER_OK)
            {
                LogError("IoTHubRegistryManager_DeleteDevice failed for SAS Based Device\r\n");
            }
        }

        if (acctInfo->x509Device.deviceId)
        {
            iothub_registrymanager_result = IoTHubRegistryManager_DeleteDevice(acctInfo->iothub_registrymanager_handle, acctInfo->x509Device.deviceId);
            if (iothub_registrymanager_result != IOTHUB_REGISTRYMANAGER_OK)
            {
                LogError("IoTHubRegistryManager_DeleteDevice failed for x509 Based Device\r\n");
            }
        }

        IoTHubMessaging_LL_Destroy(acctInfo->iothub_messaging_handle);
        IoTHubRegistryManager_Destroy(acctInfo->iothub_registrymanager_handle);
        IoTHubServiceClientAuth_Destroy(acctInfo->iothub_service_client_auth_handle);

        free(acctInfo->hostname);
        free(acctInfo->iothubName);
        free(acctInfo->iothubSuffix);
        free(acctInfo->sharedAccessKey);
        free(acctInfo->sharedAccessToken);
        free(acctInfo->keyName);
        free(acctInfo->eventhubAccessKey);
        free(acctInfo->sasDevice.deviceId);
        free(acctInfo->sasDevice.primaryAuthentication);
        free(acctInfo->sasDevice.connectionString);
        free(acctInfo->x509Device.deviceId);
        free(acctInfo->x509Device.connectionString);
        free(acctInfo);
    }
}

const char* IoTHubAccount_GetEventHubConnectionString(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    const char* result = NULL;
    IOTHUB_ACCOUNT_INFO* acctInfo = (IOTHUB_ACCOUNT_INFO*)acctHandle;
    if (acctInfo != NULL)
    {
        result = acctInfo->eventhubConnString;
    }
    return result;
}

const char* IoTHubAccount_GetIoTHubName(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    const char* result = NULL;
    IOTHUB_ACCOUNT_INFO* acctInfo = (IOTHUB_ACCOUNT_INFO*)acctHandle;
    if (acctInfo != NULL)
    {
        result = acctInfo->iothubName;
    }
    return result;
}

const char* IoTHubAccount_GetIoTHubSuffix(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    const char* result = NULL;
    IOTHUB_ACCOUNT_INFO* acctInfo = (IOTHUB_ACCOUNT_INFO*)acctHandle;
    if (acctInfo != NULL)
    {
        result = acctInfo->iothubSuffix;
    }
    return result;
}

const char* IoTHubAccount_GetEventhubListenName(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    static char listenName[64];  
    const char* value;
    
#ifndef MBED_BUILD_TIMESTAMP
    if ((value = getenv("IOTHUB_EVENTHUB_LISTEN_NAME")) == NULL)
#else
    if ((value = getMbedParameter("IOTHUB_EVENTHUB_LISTEN_NAME")) == NULL)
#endif
    {
        value = IoTHubAccount_GetIoTHubName(acctHandle); 
    }

    if (value != NULL &&
        sprintf_s(listenName, 64, "%s", value) <= 0)
    {
        LogError("Failed reading IoT Hub Event Hub listen namespace (sprintf_s failed).");
    }
    
    return listenName;
}


IOTHUB_PROVISIONED_DEVICE* IoTHubAccount_GetSASDevice(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    if (acctHandle != NULL)
    {
        return &acctHandle->sasDevice;
    }
    else
    {
        return NULL;
    }
}

IOTHUB_PROVISIONED_DEVICE* IoTHubAccount_GetX509Device(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    if (acctHandle != NULL)
    {
        return &acctHandle->x509Device;
    }
    else
    {
        return NULL;
    }
}


const char* IoTHubAccount_GetIoTHubConnString(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    if (acctHandle != NULL)
    {
        return ((IOTHUB_ACCOUNT_INFO*)acctHandle)->connString;
    }
    else
    {
        return NULL;
    }
}

const char* IoTHubAccount_GetSharedAccessSignature(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    const char* result = NULL;
    IOTHUB_ACCOUNT_INFO* acctInfo = (IOTHUB_ACCOUNT_INFO*)acctHandle;
    if (acctInfo != NULL)
    {
        if (acctInfo->sharedAccessToken != NULL)
        {
            // Reuse the sharedAccessToken if it's been created already
            result = acctInfo->sharedAccessToken;
        }
        else
        {
        time_t currentTime = time(NULL);
        size_t expiry_time = (size_t)(currentTime + 3600);

        STRING_HANDLE accessKey = STRING_construct(acctInfo->sharedAccessKey);
        STRING_HANDLE iotName = STRING_construct(acctInfo->hostname);
        STRING_HANDLE keyName = STRING_construct(acctInfo->keyName);
        if (accessKey != NULL && iotName != NULL && keyName != NULL)
        {
            STRING_HANDLE sasHandle = SASToken_Create(accessKey, iotName, keyName, expiry_time);
            if (sasHandle == NULL)
            {

                result = NULL;
            }
            else
            {
                if (mallocAndStrcpy_s(&acctInfo->sharedAccessToken, STRING_c_str(sasHandle)) != 0)
                {
                    result = NULL;
                }
                else
                {
                    result = acctInfo->sharedAccessToken;
                }
                STRING_delete(sasHandle);
            }
        }
        STRING_delete(accessKey);
        STRING_delete(iotName);
        STRING_delete(keyName);
    }
    }
    else
    {
        result = NULL;
    }
    return result; 
}

const char* IoTHubAccount_GetEventhubAccessKey(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    const char *iothub_connection_string;
    static char access_key[128];

    if ((iothub_connection_string = IoTHubAccount_GetIoTHubConnString(acctHandle)) != NULL)
    {
        STRING_HANDLE iothub_connection_string_str;

        if ((iothub_connection_string_str = STRING_construct(iothub_connection_string)) != NULL)
        {
            STRING_TOKENIZER_HANDLE tokenizer;
        
            if ((tokenizer = STRING_TOKENIZER_create(iothub_connection_string_str)) != NULL)
            {
                STRING_HANDLE tokenString;

                if ((tokenString = STRING_new()) != NULL)
                {
                    STRING_HANDLE valueString;

                    if ((valueString = STRING_new()) != NULL)
                    {
                        while ((STRING_TOKENIZER_get_next_token(tokenizer, tokenString, "=") == 0))
                        {
                            char tokenValue[128];
                            strcpy(tokenValue, STRING_c_str(tokenString));

                            if (STRING_TOKENIZER_get_next_token(tokenizer, tokenString, ";") != 0)
                            {
                                break;
                            }

                            if (strcmp(tokenValue, "SharedAccessKey") == 0)
                            {
                                strcpy(access_key, STRING_c_str(tokenString));
                                break;
                            }
                        }
                    
                        STRING_delete(valueString);
                    }

                    STRING_delete(tokenString);
                }

                STRING_TOKENIZER_destroy(tokenizer);
            }
            
            STRING_delete(iothub_connection_string_str);
        }
    } 

    return access_key;
}

const char* IoTHubAccount_GetEventhubConsumerGroup(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    (void)acctHandle;
#ifdef MBED_BUILD_TIMESTAMP
    return getMbedParameter("IOTHUB_EVENTHUB_CONSUMER_GROUP");
#else
    static char consumerGroup[64];
    char *envVarValue = getenv("IOTHUB_EVENTHUB_CONSUMER_GROUP");
    if (envVarValue != NULL)
    {
        strcpy(consumerGroup, envVarValue);
    }
    else
    { 
        strcpy(consumerGroup, DEFAULT_CONSUMER_GROUP);
    }
    return consumerGroup;
#endif
}

const size_t IoTHubAccount_GetIoTHubPartitionCount(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    int value;
    (void)acctHandle;
#ifdef MBED_BUILD_TIMESTAMP
    char* str_value;
    if ((str_value = getMbedParameter("IOTHUB_PARTITION_COUNT")) != NULL)
    {
        sscanf(str_value, "%i", &value);
    }
#else
    char *envVarValue = getenv("IOTHUB_PARTITION_COUNT");
    if (envVarValue != NULL)
    {
        value = atoi(envVarValue);
    }
    else
    {
        value = DEFAULT_PARTITION_COUNT;
    }
#endif
    return (size_t)value;
}

const IOTHUB_MESSAGING_HANDLE IoTHubAccount_GetMessagingHandle(IOTHUB_ACCOUNT_INFO_HANDLE acctHandle)
{
    IOTHUB_MESSAGING_HANDLE result = NULL;
    IOTHUB_ACCOUNT_INFO* acctInfo = (IOTHUB_ACCOUNT_INFO*)acctHandle;
    if (acctInfo != NULL)
    {
        result = acctInfo->iothub_messaging_handle;
    }
    return result;
}
