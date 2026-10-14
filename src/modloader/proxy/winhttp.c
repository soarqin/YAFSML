#include "winhttp.h"
#include "common.h"
#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static HMODULE winhttp_dll;

/* Original entry points resolved at load time. Pure pass-through exports jump
 * through these via naked stubs in proxy_stubs.asm; the COM entry points keep C
 * wrappers because proxy/common.c dispatches them by host module name. */
void *winhttp_orig_DllCanUnloadNow;
void *winhttp_orig_DllGetClassObject;
void *winhttp_orig_Private1;
void *winhttp_orig_SvchostPushServiceGlobals;
void *winhttp_orig_WinHttpAddRequestHeaders;
void *winhttp_orig_WinHttpAddRequestHeadersEx;
void *winhttp_orig_WinHttpAutoProxySvcMain;
void *winhttp_orig_WinHttpCheckPlatform;
void *winhttp_orig_WinHttpCloseHandle;
void *winhttp_orig_WinHttpConnect;
void *winhttp_orig_WinHttpConnectionDeletePolicyEntries;
void *winhttp_orig_WinHttpConnectionDeletePolicyEntriesByAppSid;
void *winhttp_orig_WinHttpConnectionDeleteProxyInfo;
void *winhttp_orig_WinHttpConnectionFreeNameList;
void *winhttp_orig_WinHttpConnectionFreeProxyInfo;
void *winhttp_orig_WinHttpConnectionFreeProxyList;
void *winhttp_orig_WinHttpConnectionGetNameList;
void *winhttp_orig_WinHttpConnectionGetProxyInfo;
void *winhttp_orig_WinHttpConnectionGetProxyList;
void *winhttp_orig_WinHttpConnectionOnlyConvert;
void *winhttp_orig_WinHttpConnectionOnlyReceive;
void *winhttp_orig_WinHttpConnectionOnlySend;
void *winhttp_orig_WinHttpConnectionSetPolicyEntries;
void *winhttp_orig_WinHttpConnectionSetProxyInfo;
void *winhttp_orig_WinHttpConnectionUpdateIfIndexTable;
void *winhttp_orig_WinHttpCrackUrl;
void *winhttp_orig_WinHttpCreateProxyList;
void *winhttp_orig_WinHttpCreateProxyManager;
void *winhttp_orig_WinHttpCreateProxyResolver;
void *winhttp_orig_WinHttpCreateProxyResult;
void *winhttp_orig_WinHttpCreateUiCompatibleProxyString;
void *winhttp_orig_WinHttpCreateUrl;
void *winhttp_orig_WinHttpDetectAutoProxyConfigUrl;
void *winhttp_orig_WinHttpFreeProxyResult;
void *winhttp_orig_WinHttpFreeProxyResultEx;
void *winhttp_orig_WinHttpFreeProxySettings;
void *winhttp_orig_WinHttpFreeProxySettingsEx;
void *winhttp_orig_WinHttpFreeQueryConnectionGroupResult;
void *winhttp_orig_WinHttpGetDefaultProxyConfiguration;
void *winhttp_orig_WinHttpGetIEProxyConfigForCurrentUser;
void *winhttp_orig_WinHttpGetProxyForUrl;
void *winhttp_orig_WinHttpGetProxyForUrlEx;
void *winhttp_orig_WinHttpGetProxyForUrlEx2;
void *winhttp_orig_WinHttpGetProxyForUrlHvsi;
void *winhttp_orig_WinHttpGetProxyResult;
void *winhttp_orig_WinHttpGetProxyResultEx;
void *winhttp_orig_WinHttpGetProxySettingsEx;
void *winhttp_orig_WinHttpGetProxySettingsResultEx;
void *winhttp_orig_WinHttpGetProxySettingsVersion;
void *winhttp_orig_WinHttpGetTunnelSocket;
void *winhttp_orig_WinHttpOpen;
void *winhttp_orig_WinHttpOpenRequest;
void *winhttp_orig_WinHttpPacJsWorkerMain;
void *winhttp_orig_WinHttpProbeConnectivity;
void *winhttp_orig_WinHttpProtocolCompleteUpgrade;
void *winhttp_orig_WinHttpProtocolReceive;
void *winhttp_orig_WinHttpProtocolSend;
void *winhttp_orig_WinHttpQueryAuthSchemes;
void *winhttp_orig_WinHttpQueryConnectionGroup;
void *winhttp_orig_WinHttpQueryDataAvailable;
void *winhttp_orig_WinHttpQueryHeaders;
void *winhttp_orig_WinHttpQueryHeadersEx;
void *winhttp_orig_WinHttpQueryOption;
void *winhttp_orig_WinHttpReadData;
void *winhttp_orig_WinHttpReadDataEx;
void *winhttp_orig_WinHttpReadProxySettings;
void *winhttp_orig_WinHttpReadProxySettingsHvsi;
void *winhttp_orig_WinHttpReceiveResponse;
void *winhttp_orig_WinHttpRefreshProxySettings;
void *winhttp_orig_WinHttpRegisterProxyChangeNotification;
void *winhttp_orig_WinHttpResetAutoProxy;
void *winhttp_orig_WinHttpResolverGetProxyForUrl;
void *winhttp_orig_WinHttpSaveProxyCredentials;
void *winhttp_orig_WinHttpSendRequest;
void *winhttp_orig_WinHttpSetCredentials;
void *winhttp_orig_WinHttpSetDefaultProxyConfiguration;
void *winhttp_orig_WinHttpSetOption;
void *winhttp_orig_WinHttpSetProxySettingsPerUser;
void *winhttp_orig_WinHttpSetSecureLegacyServersAppCompat;
void *winhttp_orig_WinHttpSetStatusCallback;
void *winhttp_orig_WinHttpSetTimeouts;
void *winhttp_orig_WinHttpTimeFromSystemTime;
void *winhttp_orig_WinHttpTimeToSystemTime;
void *winhttp_orig_WinHttpUnregisterProxyChangeNotification;
void *winhttp_orig_WinHttpWebSocketClose;
void *winhttp_orig_WinHttpWebSocketCompleteUpgrade;
void *winhttp_orig_WinHttpWebSocketQueryCloseStatus;
void *winhttp_orig_WinHttpWebSocketReceive;
void *winhttp_orig_WinHttpWebSocketSend;
void *winhttp_orig_WinHttpWebSocketShutdown;
void *winhttp_orig_WinHttpWriteData;
void *winhttp_orig_WinHttpWriteProxySettings;

HRESULT winhttp_DllCanUnloadNow(void) {
    typedef HRESULT (STDAPICALLTYPE *function_t)(void);
    return winhttp_orig_DllCanUnloadNow == NULL ? E_NOINTERFACE : ((function_t)winhttp_orig_DllCanUnloadNow)();
}

HRESULT winhttp_DllGetClassObject(REFCLSID class_id, REFIID interface_id, void **object) {
    typedef HRESULT (STDAPICALLTYPE *function_t)(REFCLSID, REFIID, void **);
    return winhttp_orig_DllGetClassObject == NULL
        ? E_NOINTERFACE
        : ((function_t)winhttp_orig_DllGetClassObject)(class_id, interface_id, object);
}

bool load_winhttp_proxy(void) {
    winhttp_dll = proxy_load_system_dll(L"winhttp.dll");
    if (winhttp_dll == NULL) return false;
    winhttp_orig_DllCanUnloadNow = (void *)GetProcAddress(winhttp_dll, "DllCanUnloadNow");
    winhttp_orig_DllGetClassObject = (void *)GetProcAddress(winhttp_dll, "DllGetClassObject");
    winhttp_orig_Private1 = (void *)GetProcAddress(winhttp_dll, "Private1");
    winhttp_orig_SvchostPushServiceGlobals = (void *)GetProcAddress(winhttp_dll, "SvchostPushServiceGlobals");
    winhttp_orig_WinHttpAddRequestHeaders = (void *)GetProcAddress(winhttp_dll, "WinHttpAddRequestHeaders");
    winhttp_orig_WinHttpAddRequestHeadersEx = (void *)GetProcAddress(winhttp_dll, "WinHttpAddRequestHeadersEx");
    winhttp_orig_WinHttpAutoProxySvcMain = (void *)GetProcAddress(winhttp_dll, "WinHttpAutoProxySvcMain");
    winhttp_orig_WinHttpCheckPlatform = (void *)GetProcAddress(winhttp_dll, "WinHttpCheckPlatform");
    winhttp_orig_WinHttpCloseHandle = (void *)GetProcAddress(winhttp_dll, "WinHttpCloseHandle");
    winhttp_orig_WinHttpConnect = (void *)GetProcAddress(winhttp_dll, "WinHttpConnect");
    winhttp_orig_WinHttpConnectionDeletePolicyEntries = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionDeletePolicyEntries");
    winhttp_orig_WinHttpConnectionDeletePolicyEntriesByAppSid = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionDeletePolicyEntriesByAppSid");
    winhttp_orig_WinHttpConnectionDeleteProxyInfo = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionDeleteProxyInfo");
    winhttp_orig_WinHttpConnectionFreeNameList = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionFreeNameList");
    winhttp_orig_WinHttpConnectionFreeProxyInfo = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionFreeProxyInfo");
    winhttp_orig_WinHttpConnectionFreeProxyList = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionFreeProxyList");
    winhttp_orig_WinHttpConnectionGetNameList = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionGetNameList");
    winhttp_orig_WinHttpConnectionGetProxyInfo = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionGetProxyInfo");
    winhttp_orig_WinHttpConnectionGetProxyList = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionGetProxyList");
    winhttp_orig_WinHttpConnectionOnlyConvert = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionOnlyConvert");
    winhttp_orig_WinHttpConnectionOnlyReceive = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionOnlyReceive");
    winhttp_orig_WinHttpConnectionOnlySend = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionOnlySend");
    winhttp_orig_WinHttpConnectionSetPolicyEntries = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionSetPolicyEntries");
    winhttp_orig_WinHttpConnectionSetProxyInfo = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionSetProxyInfo");
    winhttp_orig_WinHttpConnectionUpdateIfIndexTable = (void *)GetProcAddress(winhttp_dll, "WinHttpConnectionUpdateIfIndexTable");
    winhttp_orig_WinHttpCrackUrl = (void *)GetProcAddress(winhttp_dll, "WinHttpCrackUrl");
    winhttp_orig_WinHttpCreateProxyList = (void *)GetProcAddress(winhttp_dll, "WinHttpCreateProxyList");
    winhttp_orig_WinHttpCreateProxyManager = (void *)GetProcAddress(winhttp_dll, "WinHttpCreateProxyManager");
    winhttp_orig_WinHttpCreateProxyResolver = (void *)GetProcAddress(winhttp_dll, "WinHttpCreateProxyResolver");
    winhttp_orig_WinHttpCreateProxyResult = (void *)GetProcAddress(winhttp_dll, "WinHttpCreateProxyResult");
    winhttp_orig_WinHttpCreateUiCompatibleProxyString = (void *)GetProcAddress(winhttp_dll, "WinHttpCreateUiCompatibleProxyString");
    winhttp_orig_WinHttpCreateUrl = (void *)GetProcAddress(winhttp_dll, "WinHttpCreateUrl");
    winhttp_orig_WinHttpDetectAutoProxyConfigUrl = (void *)GetProcAddress(winhttp_dll, "WinHttpDetectAutoProxyConfigUrl");
    winhttp_orig_WinHttpFreeProxyResult = (void *)GetProcAddress(winhttp_dll, "WinHttpFreeProxyResult");
    winhttp_orig_WinHttpFreeProxyResultEx = (void *)GetProcAddress(winhttp_dll, "WinHttpFreeProxyResultEx");
    winhttp_orig_WinHttpFreeProxySettings = (void *)GetProcAddress(winhttp_dll, "WinHttpFreeProxySettings");
    winhttp_orig_WinHttpFreeProxySettingsEx = (void *)GetProcAddress(winhttp_dll, "WinHttpFreeProxySettingsEx");
    winhttp_orig_WinHttpFreeQueryConnectionGroupResult = (void *)GetProcAddress(winhttp_dll, "WinHttpFreeQueryConnectionGroupResult");
    winhttp_orig_WinHttpGetDefaultProxyConfiguration = (void *)GetProcAddress(winhttp_dll, "WinHttpGetDefaultProxyConfiguration");
    winhttp_orig_WinHttpGetIEProxyConfigForCurrentUser = (void *)GetProcAddress(winhttp_dll, "WinHttpGetIEProxyConfigForCurrentUser");
    winhttp_orig_WinHttpGetProxyForUrl = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxyForUrl");
    winhttp_orig_WinHttpGetProxyForUrlEx = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxyForUrlEx");
    winhttp_orig_WinHttpGetProxyForUrlEx2 = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxyForUrlEx2");
    winhttp_orig_WinHttpGetProxyForUrlHvsi = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxyForUrlHvsi");
    winhttp_orig_WinHttpGetProxyResult = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxyResult");
    winhttp_orig_WinHttpGetProxyResultEx = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxyResultEx");
    winhttp_orig_WinHttpGetProxySettingsEx = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxySettingsEx");
    winhttp_orig_WinHttpGetProxySettingsResultEx = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxySettingsResultEx");
    winhttp_orig_WinHttpGetProxySettingsVersion = (void *)GetProcAddress(winhttp_dll, "WinHttpGetProxySettingsVersion");
    winhttp_orig_WinHttpGetTunnelSocket = (void *)GetProcAddress(winhttp_dll, "WinHttpGetTunnelSocket");
    winhttp_orig_WinHttpOpen = (void *)GetProcAddress(winhttp_dll, "WinHttpOpen");
    winhttp_orig_WinHttpOpenRequest = (void *)GetProcAddress(winhttp_dll, "WinHttpOpenRequest");
    winhttp_orig_WinHttpPacJsWorkerMain = (void *)GetProcAddress(winhttp_dll, "WinHttpPacJsWorkerMain");
    winhttp_orig_WinHttpProbeConnectivity = (void *)GetProcAddress(winhttp_dll, "WinHttpProbeConnectivity");
    winhttp_orig_WinHttpProtocolCompleteUpgrade = (void *)GetProcAddress(winhttp_dll, "WinHttpProtocolCompleteUpgrade");
    winhttp_orig_WinHttpProtocolReceive = (void *)GetProcAddress(winhttp_dll, "WinHttpProtocolReceive");
    winhttp_orig_WinHttpProtocolSend = (void *)GetProcAddress(winhttp_dll, "WinHttpProtocolSend");
    winhttp_orig_WinHttpQueryAuthSchemes = (void *)GetProcAddress(winhttp_dll, "WinHttpQueryAuthSchemes");
    winhttp_orig_WinHttpQueryConnectionGroup = (void *)GetProcAddress(winhttp_dll, "WinHttpQueryConnectionGroup");
    winhttp_orig_WinHttpQueryDataAvailable = (void *)GetProcAddress(winhttp_dll, "WinHttpQueryDataAvailable");
    winhttp_orig_WinHttpQueryHeaders = (void *)GetProcAddress(winhttp_dll, "WinHttpQueryHeaders");
    winhttp_orig_WinHttpQueryHeadersEx = (void *)GetProcAddress(winhttp_dll, "WinHttpQueryHeadersEx");
    winhttp_orig_WinHttpQueryOption = (void *)GetProcAddress(winhttp_dll, "WinHttpQueryOption");
    winhttp_orig_WinHttpReadData = (void *)GetProcAddress(winhttp_dll, "WinHttpReadData");
    winhttp_orig_WinHttpReadDataEx = (void *)GetProcAddress(winhttp_dll, "WinHttpReadDataEx");
    winhttp_orig_WinHttpReadProxySettings = (void *)GetProcAddress(winhttp_dll, "WinHttpReadProxySettings");
    winhttp_orig_WinHttpReadProxySettingsHvsi = (void *)GetProcAddress(winhttp_dll, "WinHttpReadProxySettingsHvsi");
    winhttp_orig_WinHttpReceiveResponse = (void *)GetProcAddress(winhttp_dll, "WinHttpReceiveResponse");
    winhttp_orig_WinHttpRefreshProxySettings = (void *)GetProcAddress(winhttp_dll, "WinHttpRefreshProxySettings");
    winhttp_orig_WinHttpRegisterProxyChangeNotification = (void *)GetProcAddress(winhttp_dll, "WinHttpRegisterProxyChangeNotification");
    winhttp_orig_WinHttpResetAutoProxy = (void *)GetProcAddress(winhttp_dll, "WinHttpResetAutoProxy");
    winhttp_orig_WinHttpResolverGetProxyForUrl = (void *)GetProcAddress(winhttp_dll, "WinHttpResolverGetProxyForUrl");
    winhttp_orig_WinHttpSaveProxyCredentials = (void *)GetProcAddress(winhttp_dll, "WinHttpSaveProxyCredentials");
    winhttp_orig_WinHttpSendRequest = (void *)GetProcAddress(winhttp_dll, "WinHttpSendRequest");
    winhttp_orig_WinHttpSetCredentials = (void *)GetProcAddress(winhttp_dll, "WinHttpSetCredentials");
    winhttp_orig_WinHttpSetDefaultProxyConfiguration = (void *)GetProcAddress(winhttp_dll, "WinHttpSetDefaultProxyConfiguration");
    winhttp_orig_WinHttpSetOption = (void *)GetProcAddress(winhttp_dll, "WinHttpSetOption");
    winhttp_orig_WinHttpSetProxySettingsPerUser = (void *)GetProcAddress(winhttp_dll, "WinHttpSetProxySettingsPerUser");
    winhttp_orig_WinHttpSetSecureLegacyServersAppCompat = (void *)GetProcAddress(winhttp_dll, "WinHttpSetSecureLegacyServersAppCompat");
    winhttp_orig_WinHttpSetStatusCallback = (void *)GetProcAddress(winhttp_dll, "WinHttpSetStatusCallback");
    winhttp_orig_WinHttpSetTimeouts = (void *)GetProcAddress(winhttp_dll, "WinHttpSetTimeouts");
    winhttp_orig_WinHttpTimeFromSystemTime = (void *)GetProcAddress(winhttp_dll, "WinHttpTimeFromSystemTime");
    winhttp_orig_WinHttpTimeToSystemTime = (void *)GetProcAddress(winhttp_dll, "WinHttpTimeToSystemTime");
    winhttp_orig_WinHttpUnregisterProxyChangeNotification = (void *)GetProcAddress(winhttp_dll, "WinHttpUnregisterProxyChangeNotification");
    winhttp_orig_WinHttpWebSocketClose = (void *)GetProcAddress(winhttp_dll, "WinHttpWebSocketClose");
    winhttp_orig_WinHttpWebSocketCompleteUpgrade = (void *)GetProcAddress(winhttp_dll, "WinHttpWebSocketCompleteUpgrade");
    winhttp_orig_WinHttpWebSocketQueryCloseStatus = (void *)GetProcAddress(winhttp_dll, "WinHttpWebSocketQueryCloseStatus");
    winhttp_orig_WinHttpWebSocketReceive = (void *)GetProcAddress(winhttp_dll, "WinHttpWebSocketReceive");
    winhttp_orig_WinHttpWebSocketSend = (void *)GetProcAddress(winhttp_dll, "WinHttpWebSocketSend");
    winhttp_orig_WinHttpWebSocketShutdown = (void *)GetProcAddress(winhttp_dll, "WinHttpWebSocketShutdown");
    winhttp_orig_WinHttpWriteData = (void *)GetProcAddress(winhttp_dll, "WinHttpWriteData");
    winhttp_orig_WinHttpWriteProxySettings = (void *)GetProcAddress(winhttp_dll, "WinHttpWriteProxySettings");
    return true;
}
