#pragma once

#include "CoreMinimal.h"
#include "VeloWebViewTypes.generated.h"

/** Lifecycle state of an individual VeloWebView instance. */
UENUM(BlueprintType)
enum class EVeloWebViewState : uint8
{
	Uninitialized UMETA(DisplayName = "Uninitialized"),
	Initializing UMETA(DisplayName = "Initializing"),
	Ready        UMETA(DisplayName = "Ready"),
	Loading      UMETA(DisplayName = "Loading"),
	Failed       UMETA(DisplayName = "Failed"),
	Destroyed    UMETA(DisplayName = "Destroyed")
};

/** Permission categories exposed to Blueprint. */
UENUM(BlueprintType)
enum class EVeloWebViewPermission : uint8
{
	Camera        UMETA(DisplayName = "Camera"),
	Microphone    UMETA(DisplayName = "Microphone"),
	Geolocation   UMETA(DisplayName = "Geolocation"),
	Notifications UMETA(DisplayName = "Notifications"),
	Clipboard     UMETA(DisplayName = "Clipboard"),
	Unknown       UMETA(DisplayName = "Unknown")
};

/** Action used by navigation interception. */
UENUM(BlueprintType)
enum class EVeloWebViewNavigationAction : uint8
{
	Allow        UMETA(DisplayName = "Allow"),
	Cancel       UMETA(DisplayName = "Cancel"),
	OpenExternal UMETA(DisplayName = "Open External")
};

/** How a request for a separate browser window or tab is handled. */
UENUM(BlueprintType)
enum class EVeloWebViewNewWindowPolicy : uint8
{
	OpenInSameView = 0 UMETA(DisplayName = "Open In Same View"),
	OpenExternal   = 1 UMETA(DisplayName = "Open External Browser"),
	NotifyOnly     = 2 UMETA(DisplayName = "Notify Only"),
	Block          = 3 UMETA(DisplayName = "Block")
};

/** Structured navigation/runtime error delivered to Blueprint. */
USTRUCT(BlueprintType)
struct VELOWEBVIEW_API FVeloWebViewError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VeloWebView")
	int32 ErrorCode = 0;

	UPROPERTY(BlueprintReadOnly, Category = "VeloWebView")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "VeloWebView")
	FString URL;

	FVeloWebViewError() = default;
	FVeloWebViewError(int32 InCode, FString InDescription, FString InURL = FString())
		: ErrorCode(InCode)
		, Description(MoveTemp(InDescription))
		, URL(MoveTemp(InURL))
	{
	}
};
