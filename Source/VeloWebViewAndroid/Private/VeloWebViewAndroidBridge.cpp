#include "VeloWebViewAndroidBridge.h"

#include "Async/Async.h"

#if PLATFORM_ANDROID && USE_ANDROID_JNI

#include "Android/AndroidApplication.h"
#include "Android/AndroidJNI.h"
#include "Android/AndroidJavaEnv.h"

#include <cstdarg>

namespace
{
	FCriticalSection GCallbackMutex;
	TMap<int32, FVeloWebViewAndroidCallbacks> GCallbacks;
	jclass GManagerClass = nullptr;

	JNIEnv* GetEnv()
	{
		return FAndroidApplication::GetJavaEnv(false);
	}

	jclass GetManagerClass(JNIEnv* Env)
	{
		if (GManagerClass == nullptr && Env != nullptr)
		{
			GManagerClass = FAndroidApplication::FindJavaClassGlobalRef("com/velowebview/VeloWebViewManager");
		}
		return GManagerClass;
	}

	jmethodID FindStatic(JNIEnv* Env, const ANSICHAR* Name, const ANSICHAR* Signature)
	{
		jclass Clazz = GetManagerClass(Env);
		return (Clazz != nullptr) ? FJavaWrapper::FindStaticMethod(Env, Clazz, Name, Signature, true) : nullptr;
	}

	void ClearJavaException(JNIEnv* Env)
	{
		if (Env != nullptr && Env->ExceptionCheck())
		{
			// Do not crash a packaged game because an optional Java API is missing.
			Env->ExceptionClear();
		}
	}

	FString FromJavaString(JNIEnv* Env, jstring Value)
	{
		return (Env != nullptr && Value != nullptr) ? FJavaHelper::FStringFromLocalRef(Env, Value) : FString();
	}

	template <typename Fn>
	void Dispatch(int32 InstanceId, Fn&& Invoke)
	{
		FVeloWebViewAndroidCallbacks Callbacks;
		{
			FScopeLock Lock(&GCallbackMutex);
			if (const FVeloWebViewAndroidCallbacks* Found = GCallbacks.Find(InstanceId))
			{
				Callbacks = *Found;
			}
		}

		// The callback object is copied before queueing, so destroying the UObject
		// and clearing the map cannot leave a raw UObject callback in flight.  Core
		// adapters should capture TWeakObjectPtr/TWeakPtr in their TFunctions.
		AsyncTask(ENamedThreads::GameThread, [Callbacks = MoveTemp(Callbacks), Invoke = Forward<Fn>(Invoke)]() mutable
		{
			Invoke(Callbacks);
		});
	}

	void CallVoid0(const ANSICHAR* Name, const ANSICHAR* Signature, int32 InstanceId = INDEX_NONE)
	{
		JNIEnv* Env = GetEnv();
		jmethodID Method = FindStatic(Env, Name, Signature);
		if (Env != nullptr && Method != nullptr)
		{
			if (InstanceId == INDEX_NONE)
			{
				FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method);
			}
			else
			{
				FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method, (jint)InstanceId);
			}
			ClearJavaException(Env);
		}
	}

	void CallVoidBool(const ANSICHAR* Name, int32 InstanceId, bool Value)
	{
		JNIEnv* Env = GetEnv();
		jmethodID Method = FindStatic(Env, Name, "(IZ)V");
		if (Env != nullptr && Method != nullptr)
		{
			FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method, (jint)InstanceId, (jboolean)Value);
			ClearJavaException(Env);
		}
	}

	void CallVoidString(const ANSICHAR* Name, int32 InstanceId, const FString& Value)
	{
		JNIEnv* Env = GetEnv();
		jmethodID Method = FindStatic(Env, Name, "(ILjava/lang/String;)V");
		if (Env != nullptr && Method != nullptr)
		{
			auto JavaValue = FJavaHelper::ToJavaString(Env, Value);
			FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method, (jint)InstanceId, *JavaValue);
			ClearJavaException(Env);
		}
	}

	// UE 5.4's FJavaWrapper::CallStatic{Int,Boolean,Float}Method helpers pass
	// their va_list to the non-V JNI entry points.  On Android this makes the
	// va_list pointer itself become the first Java argument; ART then aborts
	// while validating boolean arguments.  Keep all non-void calls on the
	// correctly typed JNI V variants instead of using those wrappers.
	jint CallStaticIntV(JNIEnv* Env, jclass Clazz, jmethodID Method, ...)
	{
		if (Env == nullptr || Clazz == nullptr || Method == nullptr)
		{
			return 0;
		}

		va_list Args;
		va_start(Args, Method);
		const jint Result = Env->CallStaticIntMethodV(Clazz, Method, Args);
		va_end(Args);
		return Result;
	}

	jboolean CallStaticBooleanV(JNIEnv* Env, jclass Clazz, jmethodID Method, ...)
	{
		if (Env == nullptr || Clazz == nullptr || Method == nullptr)
		{
			return JNI_FALSE;
		}

		va_list Args;
		va_start(Args, Method);
		const jboolean Result = Env->CallStaticBooleanMethodV(Clazz, Method, Args);
		va_end(Args);
		return Result;
	}

	jfloat CallStaticFloatV(JNIEnv* Env, jclass Clazz, jmethodID Method, ...)
	{
		if (Env == nullptr || Clazz == nullptr || Method == nullptr)
		{
			return 0.0f;
		}

		va_list Args;
		va_start(Args, Method);
		const jfloat Result = Env->CallStaticFloatMethodV(Clazz, Method, Args);
		va_end(Args);
		return Result;
	}
}

bool FVeloWebViewAndroidBridge::IsAvailable()
{
	JNIEnv* Env = GetEnv();
	return Env != nullptr && GetManagerClass(Env) != nullptr;
}

int32 FVeloWebViewAndroidBridge::CreateInstance(int32 RequestedId, bool bEnableJavaScript, bool bMediaAutoplay,
	bool bMuted, bool bTransparentBackground, bool bAllowMixedContent, const FString& UserAgent)
{
	JNIEnv* Env = GetEnv();
	if (Env == nullptr)
	{
		return 0;
	}

	// The manager accepts the GameActivity object and lazily attaches its
	// overlay.  This also handles a recreated Activity without retaining it in
	// native code.
	jclass Clazz = GetManagerClass(Env);
	jmethodID Initialize = FindStatic(Env, "initialize", "(Landroid/app/Activity;)V");
	if (Clazz == nullptr || Initialize == nullptr)
	{
		return 0;
	}
	FJavaWrapper::CallStaticVoidMethod(Env, Clazz, Initialize, FJavaWrapper::GameActivityThis);
	ClearJavaException(Env);

	jmethodID Method = FindStatic(Env, "create", "(IZZZZZLjava/lang/String;)I");
	if (Method == nullptr)
	{
		return 0;
	}
	auto JavaUA = FJavaHelper::ToJavaString(Env, UserAgent);
	jint Id = CallStaticIntV(Env, Clazz, Method,
		(jint)RequestedId,
		(jint)(bEnableJavaScript ? 1 : 0),
		(jint)(bMediaAutoplay ? 1 : 0),
		(jint)(bMuted ? 1 : 0),
		(jint)(bTransparentBackground ? 1 : 0),
		(jint)(bAllowMixedContent ? 1 : 0),
		*JavaUA);
	ClearJavaException(Env);
	return (int32)Id;
}

void FVeloWebViewAndroidBridge::DestroyInstance(int32 InstanceId)
{
	CallVoid0("destroy", "(I)V", InstanceId);
	ClearCallbacks(InstanceId);
}

void FVeloWebViewAndroidBridge::SetCallbacks(int32 InstanceId, const FVeloWebViewAndroidCallbacks& Callbacks)
{
	FScopeLock Lock(&GCallbackMutex);
	GCallbacks.Add(InstanceId, Callbacks);
}

void FVeloWebViewAndroidBridge::ClearCallbacks(int32 InstanceId)
{
	FScopeLock Lock(&GCallbackMutex);
	GCallbacks.Remove(InstanceId);
}

void FVeloWebViewAndroidBridge::SetBounds(int32 InstanceId, float X, float Y, float Width, float Height, bool bCoordinatesAreDp)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "setBounds", "(IFFFFZ)V");
	if (Env != nullptr && Method != nullptr)
	{
		FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method, (jint)InstanceId,
			(jfloat)X, (jfloat)Y, (jfloat)Width, (jfloat)Height, (jboolean)bCoordinatesAreDp);
		ClearJavaException(Env);
	}
}

void FVeloWebViewAndroidBridge::SetVisibility(int32 InstanceId, bool bVisible)
{
	CallVoidBool("setVisibility", InstanceId, bVisible);
}

void FVeloWebViewAndroidBridge::SetFocus(int32 InstanceId, bool bFocused)
{
	CallVoidBool("setFocus", InstanceId, bFocused);
}

void FVeloWebViewAndroidBridge::LoadURL(int32 InstanceId, const FString& URL)
{
	CallVoidString("loadURL", InstanceId, URL);
}

void FVeloWebViewAndroidBridge::LoadHTML(int32 InstanceId, const FString& HTML, const FString& BaseURL)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "loadHTML", "(ILjava/lang/String;Ljava/lang/String;)V");
	if (Env != nullptr && Method != nullptr)
	{
		auto JavaHTML = FJavaHelper::ToJavaString(Env, HTML);
		auto JavaBase = FJavaHelper::ToJavaString(Env, BaseURL);
		FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method, (jint)InstanceId, *JavaHTML, *JavaBase);
		ClearJavaException(Env);
	}
}

void FVeloWebViewAndroidBridge::Reload(int32 InstanceId) { CallVoid0("reload", "(I)V", InstanceId); }
void FVeloWebViewAndroidBridge::Stop(int32 InstanceId) { CallVoid0("stop", "(I)V", InstanceId); }
void FVeloWebViewAndroidBridge::GoBack(int32 InstanceId) { CallVoid0("goBack", "(I)V", InstanceId); }
void FVeloWebViewAndroidBridge::GoForward(int32 InstanceId) { CallVoid0("goForward", "(I)V", InstanceId); }

bool FVeloWebViewAndroidBridge::CanGoBack(int32 InstanceId)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "canGoBack", "(I)Z");
	if (Env == nullptr || Method == nullptr) return false;
	bool Result = CallStaticBooleanV(Env, GetManagerClass(Env), Method, (jint)InstanceId) != JNI_FALSE;
	ClearJavaException(Env);
	return Result;
}

bool FVeloWebViewAndroidBridge::CanGoForward(int32 InstanceId)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "canGoForward", "(I)Z");
	if (Env == nullptr || Method == nullptr) return false;
	bool Result = CallStaticBooleanV(Env, GetManagerClass(Env), Method, (jint)InstanceId) != JNI_FALSE;
	ClearJavaException(Env);
	return Result;
}

void FVeloWebViewAndroidBridge::ExecuteJavaScript(int32 InstanceId, const FString& Script)
{
	CallVoidString("executeJavaScript", InstanceId, Script);
}

void FVeloWebViewAndroidBridge::SetJavaScriptEnabled(int32 InstanceId, bool bEnabled) { CallVoidBool("setJavaScriptEnabled", InstanceId, bEnabled); }
void FVeloWebViewAndroidBridge::SetMediaAutoplay(int32 InstanceId, bool bEnabled) { CallVoidBool("setMediaAutoplay", InstanceId, bEnabled); }
void FVeloWebViewAndroidBridge::SetMuted(int32 InstanceId, bool bMuted) { CallVoidBool("setMuted", InstanceId, bMuted); }
void FVeloWebViewAndroidBridge::SetBackgroundTransparent(int32 InstanceId, bool bTransparent) { CallVoidBool("setBackgroundTransparent", InstanceId, bTransparent); }
void FVeloWebViewAndroidBridge::SetAllowMixedContent(int32 InstanceId, bool bAllowed) { CallVoidBool("setAllowMixedContent", InstanceId, bAllowed); }
void FVeloWebViewAndroidBridge::SetNewWindowPolicy(int32 InstanceId, uint8 Policy)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "setNewWindowPolicy", "(II)V");
	if (Env != nullptr && Method != nullptr)
	{
		FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method, (jint)InstanceId, (jint)Policy);
		ClearJavaException(Env);
	}
}
void FVeloWebViewAndroidBridge::SetUserAgent(int32 InstanceId, const FString& UserAgent) { CallVoidString("setUserAgent", InstanceId, UserAgent); }

FString FVeloWebViewAndroidBridge::GetURL(int32 InstanceId)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "getURL", "(I)Ljava/lang/String;");
	if (Env == nullptr || Method == nullptr) return FString();
	jstring Value = (jstring)FJavaWrapper::CallStaticObjectMethod(Env, GetManagerClass(Env), Method, (jint)InstanceId);
	FString Result = FromJavaString(Env, Value);
	ClearJavaException(Env);
	return Result;
}

FString FVeloWebViewAndroidBridge::GetTitle(int32 InstanceId)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "getTitle", "(I)Ljava/lang/String;");
	if (Env == nullptr || Method == nullptr) return FString();
	jstring Value = (jstring)FJavaWrapper::CallStaticObjectMethod(Env, GetManagerClass(Env), Method, (jint)InstanceId);
	FString Result = FromJavaString(Env, Value);
	ClearJavaException(Env);
	return Result;
}

void FVeloWebViewAndroidBridge::SetCookie(const FString& URL, const FString& Cookie)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "setCookie", "(Ljava/lang/String;Ljava/lang/String;)V");
	if (Env != nullptr && Method != nullptr)
	{
		auto JURL = FJavaHelper::ToJavaString(Env, URL);
		auto JCookie = FJavaHelper::ToJavaString(Env, Cookie);
		FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method, *JURL, *JCookie);
		ClearJavaException(Env);
	}
}

FString FVeloWebViewAndroidBridge::GetCookie(const FString& URL)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "getCookie", "(Ljava/lang/String;)Ljava/lang/String;");
	if (Env == nullptr || Method == nullptr) return FString();
	auto JURL = FJavaHelper::ToJavaString(Env, URL);
	jstring Value = (jstring)FJavaWrapper::CallStaticObjectMethod(Env, GetManagerClass(Env), Method, *JURL);
	FString Result = FromJavaString(Env, Value);
	ClearJavaException(Env);
	return Result;
}

void FVeloWebViewAndroidBridge::DeleteCookie(const FString& URL, const FString& Name)
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "deleteCookie", "(Ljava/lang/String;Ljava/lang/String;)V");
	if (Env != nullptr && Method != nullptr)
	{
		auto JURL = FJavaHelper::ToJavaString(Env, URL);
		auto JName = FJavaHelper::ToJavaString(Env, Name);
		FJavaWrapper::CallStaticVoidMethod(Env, GetManagerClass(Env), Method, *JURL, *JName);
		ClearJavaException(Env);
	}
}

void FVeloWebViewAndroidBridge::ClearCookies()
{
	CallVoid0("clearCookies", "()V");
}

void FVeloWebViewAndroidBridge::ClearCache(int32 InstanceId)
{
	CallVoid0("clearCache", "(I)V", InstanceId);
}

void FVeloWebViewAndroidBridge::GrantPermission(int32 PermissionToken)
{
	CallVoid0("grantPermission", "(I)V", PermissionToken);
}

void FVeloWebViewAndroidBridge::DenyPermission(int32 PermissionToken)
{
	CallVoid0("denyPermission", "(I)V", PermissionToken);
}

void FVeloWebViewAndroidBridge::OnActivityPause() { CallVoid0("onActivityPause", "()V"); }
void FVeloWebViewAndroidBridge::OnActivityResume() { CallVoid0("onActivityResume", "()V"); }
void FVeloWebViewAndroidBridge::OnActivityDestroyed() { CallVoid0("onActivityDestroyed", "()V"); }

bool FVeloWebViewAndroidBridge::HandleBackPressed()
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "handleBackPressed", "()Z");
	if (Env == nullptr || Method == nullptr) return false;
	bool Result = CallStaticBooleanV(Env, GetManagerClass(Env), Method) != JNI_FALSE;
	ClearJavaException(Env);
	return Result;
}

float FVeloWebViewAndroidBridge::GetDisplayDensity()
{
	JNIEnv* Env = GetEnv();
	jmethodID Method = FindStatic(Env, "getDisplayDensity", "()F");
	if (Env == nullptr || Method == nullptr) return 1.0f;
	float Result = CallStaticFloatV(Env, GetManagerClass(Env), Method);
	ClearJavaException(Env);
	return Result > 0.0f ? Result : 1.0f;
}

// Java -> native callbacks.  These are intentionally C ABI symbols rather
// than UObject methods; Java can safely call them from WebView/UI threads.
extern "C"
{
	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnPageStarted(JNIEnv* Env, jclass, jint Id, jstring URL)
	{
		const FString Value = FromJavaString(Env, URL);
		Dispatch((int32)Id, [Value](FVeloWebViewAndroidCallbacks& C) { if (C.OnPageStarted) C.OnPageStarted(Value); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnPageLoaded(JNIEnv* Env, jclass, jint Id, jstring URL)
	{
		const FString Value = FromJavaString(Env, URL);
		Dispatch((int32)Id, [Value](FVeloWebViewAndroidCallbacks& C) { if (C.OnPageLoaded) C.OnPageLoaded(Value); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnPageError(JNIEnv* Env, jclass, jint Id, jint Code, jstring Description, jstring URL)
	{
		const FString Desc = FromJavaString(Env, Description);
		const FString Address = FromJavaString(Env, URL);
		Dispatch((int32)Id, [Code, Desc, Address](FVeloWebViewAndroidCallbacks& C) { if (C.OnPageError) C.OnPageError((int32)Code, Desc, Address); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnURLChanged(JNIEnv* Env, jclass, jint Id, jstring URL)
	{
		const FString Value = FromJavaString(Env, URL);
		Dispatch((int32)Id, [Value](FVeloWebViewAndroidCallbacks& C) { if (C.OnURLChanged) C.OnURLChanged(Value); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnTitleChanged(JNIEnv* Env, jclass, jint Id, jstring Title)
	{
		const FString Value = FromJavaString(Env, Title);
		Dispatch((int32)Id, [Value](FVeloWebViewAndroidCallbacks& C) { if (C.OnTitleChanged) C.OnTitleChanged(Value); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnJavaScriptMessage(JNIEnv* Env, jclass, jint Id, jstring Message)
	{
		const FString Value = FromJavaString(Env, Message);
		Dispatch((int32)Id, [Value](FVeloWebViewAndroidCallbacks& C) { if (C.OnJavaScriptMessage) C.OnJavaScriptMessage(Value); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnConsoleMessage(JNIEnv* Env, jclass, jint Id, jstring Message, jstring Source, jint Line)
	{
		const FString Text = FromJavaString(Env, Message);
		const FString File = FromJavaString(Env, Source);
		Dispatch((int32)Id, [Text, File, Line](FVeloWebViewAndroidCallbacks& C) { if (C.OnConsoleMessage) C.OnConsoleMessage(Text, File, (int32)Line); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnDownloadRequested(JNIEnv* Env, jclass, jint Id, jstring URL, jstring Filename, jstring Mime)
	{
		const FString Address = FromJavaString(Env, URL);
		const FString Name = FromJavaString(Env, Filename);
		const FString Type = FromJavaString(Env, Mime);
		Dispatch((int32)Id, [Address, Name, Type](FVeloWebViewAndroidCallbacks& C) { if (C.OnDownloadRequested) C.OnDownloadRequested(Address, Name, Type); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnPermissionRequested(JNIEnv* Env, jclass, jint Id, jint Token, jstring Resources, jstring Origin)
	{
		const FString ResourceList = FromJavaString(Env, Resources);
		const FString SecurityOrigin = FromJavaString(Env, Origin);
		Dispatch((int32)Id, [Token, ResourceList, SecurityOrigin](FVeloWebViewAndroidCallbacks& C) { if (C.OnPermissionRequested) C.OnPermissionRequested((int32)Token, ResourceList, SecurityOrigin); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnFullscreenChanged(JNIEnv*, jclass, jint Id, jboolean Fullscreen)
	{
		Dispatch((int32)Id, [Fullscreen](FVeloWebViewAndroidCallbacks& C) { if (C.OnFullscreenChanged) C.OnFullscreenChanged(Fullscreen != JNI_FALSE); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnBeforeNavigate(JNIEnv* Env, jclass, jint Id, jstring URL)
	{
		const FString Value = FromJavaString(Env, URL);
		Dispatch((int32)Id, [Value](FVeloWebViewAndroidCallbacks& C) { if (C.OnBeforeNavigate) C.OnBeforeNavigate(Value); });
	}

	JNIEXPORT void JNICALL Java_com_velowebview_VeloWebViewManager_nativeOnNewWindowRequested(JNIEnv* Env, jclass, jint Id, jstring URL, jboolean UserInitiated)
	{
		const FString Value = FromJavaString(Env, URL);
		Dispatch((int32)Id, [Value, bUserInitiated = UserInitiated != JNI_FALSE](FVeloWebViewAndroidCallbacks& C)
		{
			if (C.OnNewWindowRequested) C.OnNewWindowRequested(Value, bUserInitiated);
		});
	}
}

#else // PLATFORM_ANDROID && USE_ANDROID_JNI

bool FVeloWebViewAndroidBridge::IsAvailable() { return false; }
int32 FVeloWebViewAndroidBridge::CreateInstance(int32, bool, bool, bool, bool, bool, const FString&) { return 0; }
void FVeloWebViewAndroidBridge::DestroyInstance(int32) {}
void FVeloWebViewAndroidBridge::SetCallbacks(int32, const FVeloWebViewAndroidCallbacks&) {}
void FVeloWebViewAndroidBridge::ClearCallbacks(int32) {}
void FVeloWebViewAndroidBridge::SetBounds(int32, float, float, float, float, bool) {}
void FVeloWebViewAndroidBridge::SetVisibility(int32, bool) {}
void FVeloWebViewAndroidBridge::SetFocus(int32, bool) {}
void FVeloWebViewAndroidBridge::LoadURL(int32, const FString&) {}
void FVeloWebViewAndroidBridge::LoadHTML(int32, const FString&, const FString&) {}
void FVeloWebViewAndroidBridge::Reload(int32) {}
void FVeloWebViewAndroidBridge::Stop(int32) {}
void FVeloWebViewAndroidBridge::GoBack(int32) {}
void FVeloWebViewAndroidBridge::GoForward(int32) {}
bool FVeloWebViewAndroidBridge::CanGoBack(int32) { return false; }
bool FVeloWebViewAndroidBridge::CanGoForward(int32) { return false; }
void FVeloWebViewAndroidBridge::ExecuteJavaScript(int32, const FString&) {}
void FVeloWebViewAndroidBridge::SetJavaScriptEnabled(int32, bool) {}
void FVeloWebViewAndroidBridge::SetMediaAutoplay(int32, bool) {}
void FVeloWebViewAndroidBridge::SetMuted(int32, bool) {}
void FVeloWebViewAndroidBridge::SetUserAgent(int32, const FString&) {}
void FVeloWebViewAndroidBridge::SetBackgroundTransparent(int32, bool) {}
void FVeloWebViewAndroidBridge::SetAllowMixedContent(int32, bool) {}
void FVeloWebViewAndroidBridge::SetNewWindowPolicy(int32, uint8) {}
FString FVeloWebViewAndroidBridge::GetURL(int32) { return FString(); }
FString FVeloWebViewAndroidBridge::GetTitle(int32) { return FString(); }
void FVeloWebViewAndroidBridge::SetCookie(const FString&, const FString&) {}
FString FVeloWebViewAndroidBridge::GetCookie(const FString&) { return FString(); }
void FVeloWebViewAndroidBridge::DeleteCookie(const FString&, const FString&) {}
void FVeloWebViewAndroidBridge::ClearCookies() {}
void FVeloWebViewAndroidBridge::ClearCache(int32) {}
void FVeloWebViewAndroidBridge::GrantPermission(int32) {}
void FVeloWebViewAndroidBridge::DenyPermission(int32) {}
void FVeloWebViewAndroidBridge::OnActivityPause() {}
void FVeloWebViewAndroidBridge::OnActivityResume() {}
void FVeloWebViewAndroidBridge::OnActivityDestroyed() {}
bool FVeloWebViewAndroidBridge::HandleBackPressed() { return false; }
float FVeloWebViewAndroidBridge::GetDisplayDensity() { return 1.0f; }

#endif
