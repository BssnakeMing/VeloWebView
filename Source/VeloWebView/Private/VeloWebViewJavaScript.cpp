#include "VeloWebView.h"
#include "IVeloWebViewPlatform.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace VeloJavaScriptPrivate
{
	constexpr int32 MaxPendingRequests = 128;
	constexpr int32 MaxPacketBytes = 1024 * 1024 - 24;
	const FString Prefix = TEXT("__VELO_RPC__");

	// UE 5.4's JSON reader does not deserialize scalar roots. Wrap the value to
	// support null, strings, numbers and booleans as well as objects and arrays.
	bool ParseValue(const FString& JSON, TSharedPtr<FJsonValue>& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		if (JSON.Len() > MaxPacketBytes ||
			!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + JSON + TEXT("]")), Values) || Values.Num() != 1)
		{
			return false;
		}
		Value = Values[0];
		return Value.IsValid();
	}

	FString Stringify(const TSharedPtr<FJsonValue>& Value)
	{
		FString JSON;
		// UE 5.4's writer emits a leading comma for a scalar at the root and
		// cannot close that document. Serialize one array element, then unwrap it.
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(Value);
		FJsonSerializer::Serialize(Values, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JSON));
		JSON = JSON.Mid(1, JSON.Len() - 2);
		// Keep scripts compatible with older Android JavaScript parsers too.
		JSON.ReplaceInline(TEXT("\u2028"), TEXT("\\u2028"));
		JSON.ReplaceInline(TEXT("\u2029"), TEXT("\\u2029"));
		return JSON;
	}
}

bool UVeloWebView::IsJavaScriptBridgeReady() const
{
	return bEnableJavaScript && bJavaScriptBridgeReady && !JavaScriptBridgeSession.IsEmpty() && IsReady();
}

bool UVeloWebView::TryExecuteBridgeScript(const FString& Script)
{
	if (!IsJavaScriptBridgeReady()) return false;
#if PLATFORM_WINDOWS
	if (RenderMode == EVeloWebViewRenderMode::Composited)
		return CEFConnection.IsValid() && CEFConnection->ExecuteJavaScript(Script);
#endif
	ExecuteJavaScript(Script);
	return true;
}

bool UVeloWebView::SendJavaScriptBridgePacket(const TSharedRef<FJsonObject>& Packet)
{
	if (!IsJavaScriptBridgeReady()) return false;
	Packet->SetNumberField(TEXT("version"), 1);
	Packet->SetStringField(TEXT("session"), JavaScriptBridgeSession);
	const FString JSON = VeloJavaScriptPrivate::Stringify(MakeShared<FJsonValueObject>(Packet));
	const FString Quoted = VeloJavaScriptPrivate::Stringify(MakeShared<FJsonValueString>(JSON));
	const FString Script = TEXT("window.VeloWebView&&window.VeloWebView.__receive(JSON.parse(") + Quoted + TEXT("));");
	if (FTCHARToUTF8(*Script).Length() > VeloJavaScriptPrivate::MaxPacketBytes) return false;
	return TryExecuteBridgeScript(Script);
}

bool UVeloWebView::PostMessageToJavaScript(const FString& Message)
{
	const TSharedRef<FJsonObject> Packet = MakeShared<FJsonObject>();
	Packet->SetStringField(TEXT("kind"), TEXT("message"));
	Packet->SetStringField(TEXT("data"), Message);
	return SendJavaScriptBridgePacket(Packet);
}

bool UVeloWebView::PostJSONToJavaScript(const FString& MessageJSON)
{
	TSharedPtr<FJsonValue> Value;
	if (!VeloJavaScriptPrivate::ParseValue(MessageJSON, Value)) return false;
	const TSharedRef<FJsonObject> Packet = MakeShared<FJsonObject>();
	Packet->SetStringField(TEXT("kind"), TEXT("message"));
	Packet->SetField(TEXT("data"), Value);
	return SendJavaScriptBridgePacket(Packet);
}

FString UVeloWebView::ExecuteJavaScriptWithResult(const FString& Script, const FVeloWebViewJavaScriptResult& Callback, float TimeoutSeconds)
{
	if (!IsJavaScriptBridgeReady() || !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0 ||
		PendingJavaScriptResults.Num() >= VeloJavaScriptPrivate::MaxPendingRequests)
	{
		Callback.ExecuteIfBound(false, FString(), TEXT("JavaScript bridge is not ready, timeout is invalid, or request limit was reached."));
		return FString();
	}
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString QuotedSource = VeloJavaScriptPrivate::Stringify(MakeShared<FJsonValueString>(Script));
	const FString Session = VeloJavaScriptPrivate::Stringify(MakeShared<FJsonValueString>(JavaScriptBridgeSession));
	const FString Command = TEXT("window.VeloWebView&&window.VeloWebView.__evaluate(\"") + Id + TEXT("\",") + QuotedSource + TEXT(",") + Session + TEXT(");");
	if (FTCHARToUTF8(*Command).Length() > VeloJavaScriptPrivate::MaxPacketBytes)
	{
		Callback.ExecuteIfBound(false, FString(), TEXT("JavaScript request exceeds the transport size limit."));
		return FString();
	}
	FPendingJavaScriptResult Pending;
	Pending.Callback = Callback;
	Pending.Deadline = FPlatformTime::Seconds() + FMath::Min(TimeoutSeconds, 300.0f);
	PendingJavaScriptResults.Add(Id, MoveTemp(Pending));
	EnsureJavaScriptTicker();
	if (!TryExecuteBridgeScript(Command))
	{
		PendingJavaScriptResults.Remove(Id);
		Callback.ExecuteIfBound(false, FString(), TEXT("Unable to send JavaScript request to the browser."));
		return FString();
	}
	return Id;
}

bool UVeloWebView::RespondToJavaScript(const FString& RequestId, const FString& ResultJSON)
{
	const double* Deadline = PendingJavaScriptRequests.Find(RequestId);
	if (!Deadline || *Deadline <= FPlatformTime::Seconds()) return false;
	TSharedPtr<FJsonValue> Value;
	if (!VeloJavaScriptPrivate::ParseValue(ResultJSON, Value)) return false;
	const TSharedRef<FJsonObject> Packet = MakeShared<FJsonObject>();
	Packet->SetStringField(TEXT("kind"), TEXT("response"));
	Packet->SetStringField(TEXT("id"), RequestId);
	Packet->SetBoolField(TEXT("success"), true);
	Packet->SetField(TEXT("result"), Value);
	if (!SendJavaScriptBridgePacket(Packet)) return false;
	PendingJavaScriptRequests.Remove(RequestId);
	return true;
}

bool UVeloWebView::RejectJavaScriptRequest(const FString& RequestId, const FString& Error)
{
	const double* Deadline = PendingJavaScriptRequests.Find(RequestId);
	if (!Deadline || *Deadline <= FPlatformTime::Seconds()) return false;
	const TSharedRef<FJsonObject> Packet = MakeShared<FJsonObject>();
	Packet->SetStringField(TEXT("kind"), TEXT("response"));
	Packet->SetStringField(TEXT("id"), RequestId);
	Packet->SetBoolField(TEXT("success"), false);
	Packet->SetStringField(TEXT("error"), Error);
	if (!SendJavaScriptBridgePacket(Packet)) return false;
	PendingJavaScriptRequests.Remove(RequestId);
	return true;
}

void UVeloWebView::HandleJavaScriptMessage(const FString& Message)
{
	using namespace VeloJavaScriptPrivate;
	if (!Message.StartsWith(Prefix))
	{
#if !UE_BUILD_SHIPPING
		if (Message == TEXT("VeloLayerTest")) UE_LOG(LogVeloWebView, Display, TEXT("Layer test JavaScript bridge received"));
#endif
		OnJavaScriptMessage.Broadcast(Message);
		return;
	}
	TSharedPtr<FJsonObject> Packet;
	if (Message.Len() > MaxPacketBytes ||
		!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Message.Mid(Prefix.Len())), Packet) || !Packet.IsValid()) return;
	double Version = 0;
	FString Kind;
	if (!Packet->TryGetNumberField(TEXT("version"), Version) || Version != 1 || !Packet->TryGetStringField(TEXT("kind"), Kind)) return;
	FString Session;
	if (!Packet->TryGetStringField(TEXT("session"), Session) || Session.IsEmpty() || Session.Len() > 128) return;
	UE_LOG(LogVeloWebView, Verbose, TEXT("JavaScript bridge packet kind=%s ready=%d enabled=%d currentSession=%d"),
		*Kind, bJavaScriptBridgeReady ? 1 : 0, bEnableJavaScript ? 1 : 0, Session == JavaScriptBridgeSession ? 1 : 0);
	if (Kind == TEXT("ready"))
	{
		if (bJavaScriptBridgeReady && Session == JavaScriptBridgeSession) return;
		ResetJavaScriptBridge(TEXT("JavaScript context was replaced."));
		JavaScriptBridgeSession = Session;
		bJavaScriptBridgeReady = true;
		if (bEnableJavaScript) OnJavaScriptBridgeReady.Broadcast();
		return;
	}
	if (!IsJavaScriptBridgeReady() || Session != JavaScriptBridgeSession) return;
	if (Kind == TEXT("unloaded"))
	{
		ResetJavaScriptBridge(TEXT("WebView page was unloaded."));
		return;
	}
	FString Id;
	if (!Packet->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty() || Id.Len() > 128) return;
	if (Kind == TEXT("result"))
	{
		bool bSuccess = false;
		if (!Packet->TryGetBoolField(TEXT("success"), bSuccess)) return;
		FPendingJavaScriptResult Pending;
		if (!PendingJavaScriptResults.RemoveAndCopyValue(Id, Pending)) return;
		FString Error;
		Packet->TryGetStringField(TEXT("error"), Error);
		const TSharedPtr<FJsonValue> Result = Packet->TryGetField(TEXT("result"));
		const FString ResultJSON = bSuccess ? Stringify(Result.IsValid() ? Result : MakeShared<FJsonValueNull>()) : FString();
		if (Pending.Deadline <= FPlatformTime::Seconds())
			Pending.Callback.ExecuteIfBound(false, FString(), TEXT("JavaScript request timed out."));
		else
			Pending.Callback.ExecuteIfBound(bSuccess, ResultJSON, Error);
	}
	else if (Kind == TEXT("cancel"))
	{
		PendingJavaScriptRequests.Remove(Id);
	}
	else if (Kind == TEXT("request"))
	{
		FString Method;
		double TimeoutMs = 30000;
		if (!Packet->TryGetStringField(TEXT("method"), Method) || Method.TrimStartAndEnd().IsEmpty() || PendingJavaScriptRequests.Contains(Id)) return;
		Packet->TryGetNumberField(TEXT("timeoutMs"), TimeoutMs);
		if (!FMath::IsFinite(TimeoutMs) || TimeoutMs <= 0 || PendingJavaScriptRequests.Num() >= MaxPendingRequests) return;
		PendingJavaScriptRequests.Add(Id, FPlatformTime::Seconds() + FMath::Min(TimeoutMs / 1000.0, 300.0));
		EnsureJavaScriptTicker();
		if (!OnJavaScriptRequest.IsBound())
		{
			RejectJavaScriptRequest(Id, TEXT("No native JavaScript request handler is bound."));
			return;
		}
		const TSharedPtr<FJsonValue> Params = Packet->TryGetField(TEXT("params"));
		OnJavaScriptRequest.Broadcast(Id, Method, Stringify(Params.IsValid() ? Params : MakeShared<FJsonValueNull>()));
	}
}

void UVeloWebView::EnsureJavaScriptTicker()
{
	if (!JavaScriptTicker.IsValid())
	{
		JavaScriptTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this,
			[this](float DeltaSeconds) { return TickJavaScriptRequests(DeltaSeconds); }), 0.05f);
	}
}

bool UVeloWebView::TickJavaScriptRequests(float DeltaSeconds)
{
	if (!IsReady()) ResetJavaScriptBridge(TEXT("WebView disconnected."));
	const double Now = FPlatformTime::Seconds();
	TArray<FVeloWebViewJavaScriptResult> Expired;
	for (auto It = PendingJavaScriptResults.CreateIterator(); It; ++It)
	{
		if (It.Value().Deadline <= Now) { Expired.Add(It.Value().Callback); It.RemoveCurrent(); }
	}
	for (auto It = PendingJavaScriptRequests.CreateIterator(); It; ++It)
	{
		if (It.Value() <= Now) It.RemoveCurrent();
	}
	for (const auto& Callback : Expired) Callback.ExecuteIfBound(false, FString(), TEXT("JavaScript request timed out."));
	if (!PendingJavaScriptResults.Num() && !PendingJavaScriptRequests.Num())
	{
		JavaScriptTicker.Reset();
		return false;
	}
	return true;
}

void UVeloWebView::ResetJavaScriptBridge(const FString& Reason)
{
	bJavaScriptBridgeReady = false;
	JavaScriptBridgeSession.Reset();
	PendingJavaScriptRequests.Reset();
	TMap<FString, FPendingJavaScriptResult> Pending = MoveTemp(PendingJavaScriptResults);
	PendingJavaScriptResults.Reset();
	for (const auto& Entry : Pending) Entry.Value.Callback.ExecuteIfBound(false, FString(), Reason);
}
