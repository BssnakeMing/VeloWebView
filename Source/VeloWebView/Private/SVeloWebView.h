#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class UVeloWebView;

/** Slate host for the platform-native WebView overlay. */
class SVeloWebView final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVeloWebView) {}
		SLATE_ARGUMENT(UVeloWebView*, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	TWeakObjectPtr<UVeloWebView> Owner;
	FIntRect LastGeometry;
	float LastScale = 0.0f;
	bool bLastNativeVisibility = false;
};
