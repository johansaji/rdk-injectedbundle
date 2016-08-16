#include "Proxy.h"
#include "JavaScriptRequests.h"
#include "utils.h"

#include <JavaScriptCore/JSObjectRef.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <WebKit/WKArray.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKNumber.h>
#include <WebKit/WKRetainPtr.h>
#include <fstream>

namespace JSBridge
{

namespace
{

std::string toStdString(WKStringRef string)
{
    size_t size = WKStringGetMaximumUTF8CStringSize(string);
    auto buffer = std::make_unique<char[]>(size);
    size_t len = WKStringGetUTF8CString(string, buffer.get(), size);

    return std::string(buffer.get(), len - 1);
}

void injectWPEQuery(JSGlobalContextRef context)
{
    JSObjectRef windowObject = JSContextGetGlobalObject(context);

    JSRetainPtr<JSStringRef> queryStr = adopt(JSStringCreateWithUTF8CString("wpeQuery"));
    JSValueRef wpeObject = JSObjectMakeFunctionWithCallback(context,
        queryStr.get(), JSBridge::onJavaScriptBridgeRequest);

    JSValueRef exc = 0;
    JSObjectSetProperty(context, windowObject, queryStr.get(), wpeObject,
        kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete | kJSPropertyAttributeDontEnum, &exc);

    if (exc)
    {
        fprintf(stderr, "Error: Could not set property wpeQuery!\n");
    }
}

void injectServiceManager(JSGlobalContextRef context)
{
    JSObjectRef windowObject = JSContextGetGlobalObject(context);
    JSRetainPtr<JSStringRef> serviceManagerStr = adopt(JSStringCreateWithUTF8CString("ServiceManager"));

    const char* jsFile = "/usr/share/injectedbundle/ServiceManager.js";
    std::string content;
    if (!Utils::readFile(jsFile, content))
    {
        fprintf(stderr, "Error: Could not read file %s!\n", jsFile);
        return;
    }

    JSValueRef exc = 0;
    (void) Utils::evaluateUserScript(context, content, &exc);
    if (exc)
    {
        fprintf(stderr, "Error: Could not evaluate user script %s!\n", jsFile);
        return;
    }

    if (JSObjectHasProperty(context, windowObject, serviceManagerStr.get()) != true)
    {
        fprintf(stderr, "Error: Could not find ServiceManager object!\n");
        return;
    }

    JSValueRef smObject = JSObjectGetProperty(context, windowObject, serviceManagerStr.get(), &exc);
    if (exc)
    {
        fprintf(stderr, "Error: Could not get property ServiceManager!\n");
        return;
    }

    JSRetainPtr<JSStringRef> sendQueryStr = adopt(JSStringCreateWithUTF8CString("sendQuery"));
    JSValueRef sendQueryObject = JSObjectMakeFunctionWithCallback(context,
        sendQueryStr.get(), JSBridge::onJavaScriptServiceManagerRequest);

    JSObjectSetProperty(context, (JSObjectRef) smObject, sendQueryStr.get(), sendQueryObject,
        kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete | kJSPropertyAttributeDontEnum, &exc);

    if (exc)
    {
        fprintf(stderr, "Error: Could not set property ServiceManager.sendQuery!\n");
    }
}

} // namespace

struct QueryCallbacks
{
    QueryCallbacks(JSValueRef succ, JSValueRef err)
        : onSuccess(succ)
        , onError(err)
    {}

    void protect(JSContextRef ctx)
    {
        JSValueProtect(ctx, onSuccess);
        JSValueProtect(ctx, onError);
    }

    void unprotect(JSContextRef ctx)
    {
        JSValueUnprotect(ctx, onSuccess);
        JSValueUnprotect(ctx, onError);
    }

    JSValueRef onSuccess;
    JSValueRef onError;
};

Proxy::Proxy()
{
}

void Proxy::didCommitLoad(WKBundlePageRef page, WKBundleFrameRef frame)
{
    if (WKBundlePageGetMainFrame(page) != frame)
    {
        fprintf(stdout, "%s:%d Frame is not allowed to inject JavaScript window objects!\n", __func__, __LINE__);
        return;
    }

    // Always inject wpeQuery and ServiceManager to be visible in JavaScript.
    auto context = WKBundleFrameGetJavaScriptContext(frame);
    injectWPEQuery(context);
    injectServiceManager(context);
}

void Proxy::sendQuery(const char* name, JSContextRef ctx,
    JSStringRef messageRef, JSValueRef onSuccess, JSValueRef onError)
{
    WKRetainPtr<WKStringRef> mesRef = adoptWK(WKStringCreateWithJSString(messageRef));
    std::string message = toStdString(mesRef.get());
    uint64_t callID = ++m_lastCallID;

    auto cb = new QueryCallbacks(onSuccess, onError);
    cb->protect(ctx);

    m_queries[callID].reset(cb);

    sendMessageToClient(name, message.c_str(), callID);
}

void Proxy::onMessageFromClient(WKBundlePageRef page, WKStringRef messageName, WKTypeRef messageBody)
{
    if (WKStringIsEqualToUTF8CString(messageName, "onJavaScriptBridgeResponse"))
    {
        onJavaScriptBridgeResponse(page, messageBody);
        return;
    }

    fprintf(stderr, "%s:%d Error: Unknown message name!\n", __func__, __LINE__);
}

void Proxy::onJavaScriptBridgeResponse(WKBundlePageRef page, WKTypeRef messageBody)
{
    if (WKGetTypeID(messageBody) != WKArrayGetTypeID())
    {
        fprintf(stderr, "%s:%d Error: Message body must be array!\n", __func__, __LINE__);
        return;
    }

    uint64_t callID = WKUInt64GetValue((WKUInt64Ref) WKArrayGetItemAtIndex((WKArrayRef) messageBody, 0));
    bool success = WKBooleanGetValue((WKBooleanRef) WKArrayGetItemAtIndex((WKArrayRef) messageBody, 1));
    std::string message = toStdString((WKStringRef) WKArrayGetItemAtIndex((WKArrayRef) messageBody, 2));

    fprintf(stdout, "%s:%d callID=%llu succes=%d message=%s\n", __func__, __LINE__, callID, success, message.c_str());

    auto it = m_queries.find(callID);
    if (it == m_queries.end())
    {
        fprintf(stderr, "%s:%d Error: callID=%llu not found\n", __func__, __LINE__, callID);
        return;
    }

    JSGlobalContextRef context = WKBundleFrameGetJavaScriptContext(WKBundlePageGetMainFrame(page));
    JSValueRef cb = success ? it->second->onSuccess : it->second->onError;

    const size_t argc = 1;
    JSValueRef argv[argc];
    JSRetainPtr<JSStringRef> string = adopt(JSStringCreateWithUTF8CString(message.c_str()));
    argv[0] = JSValueMakeString(context, string.get());
    (void) JSObjectCallAsFunction(context, (JSObjectRef) cb, nullptr, argc, argv, nullptr);

    it->second->unprotect(context);

    m_queries.erase(it);
}

void Proxy::sendMessageToClient(const char* name, const char* message, uint64_t callID)
{
    fprintf(stdout, "%s:%d name=%s callID=%llu message=%s\n", __func__, __LINE__, name, callID, message);

    WKRetainPtr<WKStringRef> nameRef = adoptWK(WKStringCreateWithUTF8CString(name));
    WKRetainPtr<WKUInt64Ref> callIDRef = adoptWK(WKUInt64Create(callID));
    WKRetainPtr<WKStringRef> bodyRef = adoptWK(WKStringCreateWithUTF8CString(message));

    WKTypeRef params[] = {callIDRef.get(), bodyRef.get()};
    WKRetainPtr<WKArrayRef> arrRef = adoptWK(WKArrayCreate(params, sizeof(params)/sizeof(params[0])));

    WKBundlePagePostMessage(m_client, nameRef.get(), arrRef.get());
}

Proxy& Proxy::singleton()
{
    static Proxy& singleton = *new Proxy();
    return singleton;
}

void Proxy::setClient(WKBundlePageRef bundle)
{
    m_client = bundle;
}

} // namespace WPEQuery