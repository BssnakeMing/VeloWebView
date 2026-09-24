#include "SVeloWebView.h"

#include "IVeloWebViewPlatform.h"
#include "VeloWebView.h"

#include "Widgets/Text/STextBlock.h"

void SVeloWebView::Construct(const FArguments& InArgs)
{
	Owner = InArgs._Owner;

	ChildSlot
	[
		SNew(STextBlock)
		.Visibility(EVisibility::HitTestInvisible)
		.Justification(ETextJustify::Center)
		.Text_Lambda([WeakOwner = Owner]() -> FText
		{
			if (const UVeloWebView* Widget = WeakOwner.Get())
			{
				if (Widget->IsDesignTime())
				{
					const FString URL = Widget->InitialURL.IsEmpty() ? TEXT("about:blank") : Widget->InitialURL;
					return FText::FromString(FString::Printf(TEXT("Velo WebView\n%s"), *URL));
				}
			}
			return FText::GetEmpty();
		})
	];

}

void SVeloWebView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime,
	const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	UVeloWebView* Widget = Owner.Get();
	if (!Widget || Widget->IsDesignTime())
	{
		return;
	}
	if (!Widget->Platform.IsValid())
	{
		Widget->EnsurePlatform();
	}
	if (!Widget->Platform.IsValid())
	{
		return;
	}

	const FVector2D AbsolutePosition = AllottedGeometry.GetAbsolutePosition();
	const FVector2D AbsoluteSize = AllottedGeometry.GetLocalSize() * AllottedGeometry.Scale;
	const FIntRect NewGeometry(
		FMath::RoundToInt(AbsolutePosition.X),
		FMath::RoundToInt(AbsolutePosition.Y),
		FMath::RoundToInt(AbsolutePosition.X + AbsoluteSize.X),
		FMath::RoundToInt(AbsolutePosition.Y + AbsoluteSize.Y));
	const float Scale = FMath::Max(AllottedGeometry.Scale, 0.01f);

	const bool bGeometryChanged = NewGeometry != LastGeometry || !FMath::IsNearlyEqual(Scale, LastScale);
	const bool bVisible = Widget->bRequestedVisibility && GetVisibility() != EVisibility::Collapsed && GetVisibility() != EVisibility::Hidden;
	if (bGeometryChanged)
	{
		LastGeometry = NewGeometry;
		LastScale = Scale;
		Widget->Platform->SetGeometry(NewGeometry, Scale);
	}
	if (bVisible != bLastNativeVisibility)
	{
		bLastNativeVisibility = bVisible;
		Widget->Platform->SetVisibility(bVisible);
	}
}

FReply SVeloWebView::OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent)
{
	if (UVeloWebView* Widget = Owner.Get())
	{
		if (Widget->Platform.IsValid())
		{
			Widget->Platform->SetFocus(true);
		}
	}
	return FReply::Handled();
}

FReply SVeloWebView::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (UVeloWebView* Widget = Owner.Get())
	{
		if (Widget->Platform.IsValid())
		{
			Widget->Platform->SetFocus(true);
		}
	}
	return SCompoundWidget::OnMouseButtonDown(MyGeometry, MouseEvent);
}
