#pragma once
#include "ExternalDrop.hpp"
#include <Windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>

namespace OBSWeb {
inline std::optional<ExternalDropData> ReadExternalDrop(ICoreWebView2WebMessageReceivedEventArgs *event,
						      const QJsonObject &args, QString &error)
{
	using Microsoft::WRL::ComPtr;
	QStringList paths;
	ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> extra;
	if (SUCCEEDED(event->QueryInterface(IID_PPV_ARGS(&extra)))) {
		ComPtr<ICoreWebView2ObjectCollectionView> objects;
		if (FAILED(extra->get_AdditionalObjects(&objects))) {
			error = QStringLiteral("Cannot read dropped file objects."); return std::nullopt;
		}
		if (objects) {
			UINT count = 0;
			if (FAILED(objects->get_Count(&count)) || count > 128) {
				error = QStringLiteral("Too many dropped file objects."); return std::nullopt;
			}
			for (UINT i = 0; i < count; ++i) {
				ComPtr<IUnknown> object;
				ComPtr<ICoreWebView2File> file;
				if (FAILED(objects->GetValueAtIndex(i, &object)) || !object || FAILED(object.As(&file))) {
					error = QStringLiteral("A dropped object is not a native file."); return std::nullopt;
				}
				LPWSTR path = nullptr;
				const HRESULT result = file->get_Path(&path);
				const size_t length = path ? wcsnlen_s(path, 32768) : 0;
				if (FAILED(result) || length == 0 || length >= 32768) {
					CoTaskMemFree(path);
					error = QStringLiteral("Cannot read the dropped file path."); return std::nullopt;
				}
				paths.append(QString::fromWCharArray(path, int(length)));
				CoTaskMemFree(path);
			}
		}
	}
	return ParseExternalDrop(args, paths, error);
}
}
