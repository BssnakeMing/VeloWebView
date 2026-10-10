#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Styling/SlateBrush.h"

class UVeloWebView;
#if PLATFORM_ANDROID
class SWebBrowserView;
class SVirtualWindow;
#endif

/** Slate host for the platform-native WebView overlay. */
class SVeloWebView final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVeloWebView) {}
		SLATE_ARGUMENT(UVeloWebView*, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	#if PLATFORM_ANDROID
	TSharedPtr<SWebBrowserView> GetCompositedBrowser() const { return Browser; }
	#endif

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
		const float InDeltaTime) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;
	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnTouchStarted(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent) override;
	virtual FReply OnTouchMoved(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent) override;
	virtual FReply OnTouchEnded(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent) override;
	virtual FReply OnTouchForceChanged(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent) override;
	virtual FReply OnTouchFirstMove(const FGeometry& MyGeometry, const FPointerEvent& TouchEvent) override;
	virtual FReply OnTouchGesture(const FGeometry& MyGeometry, const FPointerEvent& GestureEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
	virtual void OnFocusLost(const FFocusEvent& InFocusEvent) override;

private:
	/**
	 * Returns the browser's actual SViewport input leaf.  Normally Slate routes
	 * events to this child before they reach SVeloWebView.  Keeping the fallback
	 * here also covers hit-test paths produced by UMG wrappers that terminate at
	 * the outer widget.
	 */
	TSharedPtr<SWidget> GetCompositedInputWidget() const;

	TWeakObjectPtr<UVeloWebView> Owner;
	#if PLATFORM_ANDROID
	TSharedPtr<SWebBrowserView> Browser;
	TSharedPtr<SVirtualWindow> BrowserVirtualWindow;
	#endif
	FIntRect LastGeometry;
	float LastScale = 0.0f;
	bool bLastNativeVisibility = false;
	bool bLastCEFConnected = false;
	FSlateBrush CEFBrush;
#if PLATFORM_WINDOWS
	uint8 CEFPressedButtons = 0;
	uint8 CEFClickCounts[3] = {1, 1, 1};
	FIntPoint LastCEFPointerPosition = FIntPoint::ZeroValue;
#endif
};
