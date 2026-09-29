// Disposable, local D3D11 target for OBS Game Capture and Window Capture QA.
// It renders its own color pattern; it never hooks or reads another process.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>

using Microsoft::WRL::ComPtr;
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_APP + 10) {
		NotifyWinEvent(EVENT_SYSTEM_MOVESIZESTART, window, OBJID_WINDOW, CHILDID_SELF);
		SetWindowPos(window, nullptr, int(wparam), int(lparam), 0, 0,
			     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		NotifyWinEvent(EVENT_SYSTEM_MOVESIZEEND, window, OBJID_WINDOW, CHILDID_SELF);
		return 0;
	}
	if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
	if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
	return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = WindowProc;
	wc.lpszClassName = L"OBSWebViewCaptureFixture";
	RegisterClassW(&wc);
	RECT bounds{0, 0, 640, 360}; AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
	auto window = CreateWindowW(wc.lpszClassName, L"OBS capture QA target", WS_OVERLAPPEDWINDOW,
		80, 100, bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
	if (!window) return 2;
	DXGI_SWAP_CHAIN_DESC desc{};
	desc.BufferDesc.Width = 640; desc.BufferDesc.Height = 360;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2; desc.OutputWindow = window; desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGISwapChain> swap;
	if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device, nullptr, &context))) return 3;
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11RenderTargetView> target;
	if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&texture))) || FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &target))) return 4;
	ShowWindow(window, SW_SHOWNOACTIVATE);
	const auto started = GetTickCount64();
	bool running = true;
	while (running && GetTickCount64() - started < 600000) {
		MSG message;
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			if (message.message == WM_QUIT) running = false;
			TranslateMessage(&message); DispatchMessageW(&message);
		}
		const float phase = float(GetTickCount64() - started) / 1500.f;
		const float color[]{.1f + .1f * std::sin(phase), .35f, .65f + .15f * std::cos(phase), 1.f};
		context->ClearRenderTargetView(target.Get(), color);
		swap->Present(1, 0);
	}
	if (IsWindow(window)) DestroyWindow(window);
	return 0;
}
