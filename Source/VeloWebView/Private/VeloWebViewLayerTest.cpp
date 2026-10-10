#include "VeloWebView.h"

#if !UE_BUILD_SHIPPING && PLATFORM_WINDOWS

#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "UnrealClient.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	TWeakObjectPtr<UOverlay> LayerTestOverlay;
	TWeakObjectPtr<UVeloWebView> LayerTestWebView;
	TSharedPtr<SWidget> LayerTestWidget;

	void RunLayerTest()
	{
		if (!GEngine || !GEngine->GameViewport || LayerTestOverlay.IsValid())
		{
			UE_LOG(LogVeloWebView, Warning, TEXT("Layer test could not create the UMG overlay"));
			return;
		}

		UOverlay* Overlay = NewObject<UOverlay>();
		Overlay->AddToRoot();
		LayerTestOverlay = Overlay;

		UVeloWebView* WebView = NewObject<UVeloWebView>(Overlay);
		LayerTestWebView = WebView;
		WebView->RenderMode = EVeloWebViewRenderMode::Composited;
		WebView->InitialURL = TEXT("about:blank");
		UOverlaySlot* WebSlot = Overlay->AddChildToOverlay(WebView);
		WebSlot->SetHorizontalAlignment(HAlign_Fill);
		WebSlot->SetVerticalAlignment(VAlign_Fill);

		USizeBox* BarSize = NewObject<USizeBox>(Overlay);
		BarSize->SetHeightOverride(96.0f);
		UBorder* Bar = NewObject<UBorder>(BarSize);
		Bar->SetBrushColor(FLinearColor(0.04f, 0.65f, 0.18f, 1.0f));
		UTextBlock* Label = NewObject<UTextBlock>(Bar);
		Label->SetText(FText::FromString(TEXT("UMG TOP LAYER - WEBVIEW BELOW")));
		Label->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Bar->SetContent(Label);
		BarSize->SetContent(Bar);
		UOverlaySlot* BarSlot = Overlay->AddChildToOverlay(BarSize);
		BarSlot->SetVerticalAlignment(VAlign_Top);

		LayerTestWidget = Overlay->TakeWidget();
		GEngine->GameViewport->AddViewportWidgetContent(LayerTestWidget.ToSharedRef(), 1000);
		UE_LOG(LogVeloWebView, Display, TEXT("Layer test UMG overlay attached"));
		// Keep the page self-contained so this command tests the complete
		// Slate -> CEF -> DOM input path without depending on a project widget.
		// The DOM itself reports clicks, editing, wheel and drag events. This
		// remains useful even while the external host's JS bridge is incomplete.
		WebView->LoadHTML(TEXT(R"VELOHTML(
<html><head><meta charset="utf-8"><style>
html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#193eaa;color:white;font:28px sans-serif}
body{padding:100px 30px 12px;box-sizing:border-box}#target{font:28px sans-serif;padding:18px 36px;border:4px solid white;border-radius:12px;background:#d13b3b;color:white;cursor:pointer}#status{margin:12px 0;white-space:pre;font:20px monospace}input[type=text]{font:24px sans-serif;width:550px;padding:8px}#edit,#keys,#dragstatus,#scrollstatus{font:20px monospace;margin:8px 0}#drag{width:550px}#scroll{width:550px;height:120px;overflow:auto;border:2px solid white;font:20px sans-serif}#scroll>div{height:800px;background:linear-gradient(#2757c9,#8d2355);padding:12px}
</style></head><body><div>COMPOSITED INPUT TEST</div><button id="target">CLICK TARGET / RIGHT CLICK</button><div id="status">waiting for DOM input...</div><input id="text" type="text" placeholder="Type text / Ctrl+A / Backspace"><div id="edit">text: (empty)</div><div id="keys">key: (none)</div><input id="drag" type="range" min="0" max="100" value="20"><div id="dragstatus">drag: 20</div><div id="scroll"><div>Wheel here to scroll<br>BEGIN<br><br>Scroll content</div></div><div id="scrollstatus">scroll: 0 wheel: 0</div><script>
(function(){var n=0,s=document.getElementById('status'),b=document.getElementById('target'),w=0,sc=document.getElementById('scroll');
function report(t,e){n++;s.textContent=t+' #'+n+' x='+Math.round(e.clientX)+' y='+Math.round(e.clientY);b.style.background=t==='contextmenu'?'#7b2fc1':(t==='click'?'#168a43':'#d13b3b');if(window.VeloWebView)window.VeloWebView.postMessage('VeloInput:'+t+':'+n+':'+Math.round(e.clientX)+':'+Math.round(e.clientY));}
['mousedown','mouseup','click','contextmenu','dblclick'].forEach(function(t){document.addEventListener(t,function(e){if(t==='contextmenu')e.preventDefault();report(t,e)},true)});
document.getElementById('text').addEventListener('input',function(e){document.getElementById('edit').textContent='text: '+(e.target.value||'(empty)')});
['keydown','keyup'].forEach(function(t){document.addEventListener(t,function(e){document.getElementById('keys').textContent='key: '+t+' '+e.key+' code='+e.code+' ctrl='+e.ctrlKey+' shift='+e.shiftKey})});
document.getElementById('drag').addEventListener('input',function(e){document.getElementById('dragstatus').textContent='drag: '+e.target.value});
function scrollStatus(){document.getElementById('scrollstatus').textContent='scroll: '+Math.round(sc.scrollTop)+' wheel: '+w}
sc.addEventListener('wheel',function(){w++;scrollStatus()});sc.addEventListener('scroll',scrollStatus);
setTimeout(function(){if(window.VeloWebView)window.VeloWebView.postMessage('VeloLayerTest');},1500);
})();</script></body></html>
)VELOHTML"), TEXT("http://velowebview.local/"));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			if (UVeloWebView* View = LayerTestWebView.Get())
			{
				UE_LOG(LogVeloWebView, Display, TEXT("Layer test browser state=%d ready=%d URL=%s"),
					static_cast<int32>(View->GetState()), View->IsReady() ? 1 : 0, *View->GetURL());
			}
			FScreenshotRequest::RequestScreenshot(TEXT("VeloWebViewLayerTest.png"), true, false);
			UE_LOG(LogVeloWebView, Display, TEXT("Layer test screenshot requested"));
			return false;
		}), 5.0f);
	}

	FAutoConsoleCommand LayerTestCommand(
		TEXT("VeloWebView.LayerTest"),
		TEXT("Show a WebView under a higher UMG widget and save a screenshot."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				RunLayerTest();
				return false;
			}), 2.0f);
		}));
}

#endif
