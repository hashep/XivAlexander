// #define _XIVRES_FONTGENERATOR_DIRECTWRITEFIXEDSIZEFONT_H_

#ifndef _XIVRES_FONTGENERATOR_DIRECTWRITEFIXEDSIZEFONT_H_
#define _XIVRES_FONTGENERATOR_DIRECTWRITEFIXEDSIZEFONT_H_

#include <filesystem>
#include <numeric>

#include "IFixedSizeFont.h"

#include "../Internal/BitmapCopy.h"
#include "../Internal/TrueTypeUtils.h"

#include <comdef.h>
#include <dwrite_3.h>
#pragma comment(lib, "dwrite.lib")

_COM_SMARTPTR_TYPEDEF(IDWriteFactory, __uuidof(IDWriteFactory));
_COM_SMARTPTR_TYPEDEF(IDWriteFactory3, __uuidof(IDWriteFactory3));
_COM_SMARTPTR_TYPEDEF(IDWriteFont, __uuidof(IDWriteFont));
_COM_SMARTPTR_TYPEDEF(IDWriteFontCollection, __uuidof(IDWriteFontCollection));
_COM_SMARTPTR_TYPEDEF(IDWriteFontFace, __uuidof(IDWriteFontFace));
_COM_SMARTPTR_TYPEDEF(IDWriteFontFace1, __uuidof(IDWriteFontFace1));
_COM_SMARTPTR_TYPEDEF(IDWriteFontFaceReference, __uuidof(IDWriteFontFaceReference));
_COM_SMARTPTR_TYPEDEF(IDWriteFontFamily, __uuidof(IDWriteFontFamily));
_COM_SMARTPTR_TYPEDEF(IDWriteFontFile, __uuidof(IDWriteFontFile));
_COM_SMARTPTR_TYPEDEF(IDWriteFontFileLoader, __uuidof(IDWriteFontFileLoader));
_COM_SMARTPTR_TYPEDEF(IDWriteFontFileStream, __uuidof(IDWriteFontFileStream));
_COM_SMARTPTR_TYPEDEF(IDWriteFontSetBuilder, __uuidof(IDWriteFontSetBuilder));
_COM_SMARTPTR_TYPEDEF(IDWriteGdiInterop, __uuidof(IDWriteGdiInterop));
_COM_SMARTPTR_TYPEDEF(IDWriteGlyphRunAnalysis, __uuidof(IDWriteGlyphRunAnalysis));
_COM_SMARTPTR_TYPEDEF(IDWriteLocalizedStrings, __uuidof(IDWriteLocalizedStrings));

namespace XivRes::FontGenerator {
	static HRESULT SuccessOrThrow(HRESULT hr, std::initializer_list<HRESULT> acceptables = {}) {
		if (SUCCEEDED(hr))
			return hr;

		for (const auto& h : acceptables) {
			if (h == hr)
				return hr;
		}

		const auto err = _com_error(hr);
		wchar_t* pszMsg = nullptr;
		FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
						  FORMAT_MESSAGE_FROM_SYSTEM |
						  FORMAT_MESSAGE_IGNORE_INSERTS,
					  nullptr,
					  hr,
					  MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
					  reinterpret_cast<LPWSTR>(&pszMsg),
					  0,
					  NULL);
		if (pszMsg) {
			std::unique_ptr<wchar_t, decltype(LocalFree)*> pszMsgFree(pszMsg, LocalFree);

			throw std::runtime_error(std::format(
				"DirectWrite Error (HRESULT=0x{:08X}): {}",
				static_cast<uint32_t>(hr),
				Unicode::Convert<std::string>(std::wstring(pszMsg))
			));

		} else {
			throw std::runtime_error(std::format(
				"DirectWrite Error (HRESULT=0x{:08X})",
				static_cast<uint32_t>(hr),
				Unicode::Convert<std::string>(std::wstring(pszMsg))
			));
		}
	}

	class DirectWriteFixedSizeFont : public DefaultAbstractFixedSizeFont {
		class IStreamBasedDWriteFontFileLoader : public IDWriteFontFileLoader {
			class IStreamAsDWriteFontFileStream : public IDWriteFontFileStream {
				const std::shared_ptr<IStream> m_stream;

				std::atomic_uint32_t m_nRef = 1;

				IStreamAsDWriteFontFileStream(std::shared_ptr<IStream> pStream)
					: m_stream(std::move(pStream)) {}

			public:
				static IStreamAsDWriteFontFileStream* New(std::shared_ptr<IStream> pStream) {
					return new IStreamAsDWriteFontFileStream(std::move(pStream));
				}

				HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override {
					if (riid == __uuidof(IUnknown))
						*ppvObject = static_cast<IUnknown*>(this);
					else if (riid == __uuidof(IDWriteFontFileStream))
						*ppvObject = static_cast<IDWriteFontFileStream*>(this);
					else
						*ppvObject = nullptr;

					if (!*ppvObject)
						return E_NOINTERFACE;

					AddRef();
					return S_OK;
				}

				ULONG __stdcall AddRef(void) override {
					return ++m_nRef;
				}

				ULONG __stdcall Release(void) override {
					const auto newRef = --m_nRef;
					if (!newRef)
						delete this;
					return newRef;
				}

				HRESULT __stdcall ReadFileFragment(void const** pFragmentStart, uint64_t fileOffset, uint64_t fragmentSize, void** pFragmentContext) override {
					*pFragmentContext = nullptr;
					*pFragmentStart = nullptr;

					if (const auto pMemoryStream = dynamic_cast<MemoryStream*>(m_stream.get())) {
						try {
							*pFragmentStart = pMemoryStream->View(static_cast<std::streamoff>(fileOffset), static_cast<std::streamsize>(fragmentSize)).data();
							return S_OK;
						} catch (const std::out_of_range&) {
							return E_INVALIDARG;
						}

					} else {
						const auto size = static_cast<uint64_t>(m_stream->StreamSize());
						if (fileOffset <= size && fileOffset + fragmentSize <= size && fragmentSize <= (std::numeric_limits<size_t>::max)()) {
							auto pVec = new std::vector<uint8_t>();
							try {
								pVec->resize(static_cast<size_t>(fragmentSize));
								if (m_stream->ReadStreamPartial(static_cast<std::streamoff>(fileOffset), pVec->data(), static_cast<std::streamsize>(fragmentSize)) == fragmentSize) {
									*pFragmentStart = pVec->data();
									*pFragmentContext = pVec;
									return S_OK;
								}
							} catch (...) {
								// pass
							}
							delete pVec;
							return E_FAIL;

						} else
							return E_INVALIDARG;
					}
				}

				void __stdcall ReleaseFileFragment(void* fragmentContext) override {
					if (fragmentContext)
						delete static_cast<std::vector<uint8_t>*>(fragmentContext);
				}

				HRESULT __stdcall GetFileSize(uint64_t* pFileSize) override {
					*pFileSize = static_cast<uint16_t>(m_stream->StreamSize());
					return S_OK;
				}

				HRESULT __stdcall GetLastWriteTime(uint64_t* pLastWriteTime) override {
					*pLastWriteTime = 0;
					return E_NOTIMPL; // E_NOTIMPL by design -- see method documentation in dwrite.h.
				}
			};

			IStreamBasedDWriteFontFileLoader() = default;
			IStreamBasedDWriteFontFileLoader(IStreamBasedDWriteFontFileLoader&&) = delete;
			IStreamBasedDWriteFontFileLoader(const IStreamBasedDWriteFontFileLoader&) = delete;
			IStreamBasedDWriteFontFileLoader& operator=(IStreamBasedDWriteFontFileLoader&&) = delete;
			IStreamBasedDWriteFontFileLoader& operator=(const IStreamBasedDWriteFontFileLoader&) = delete;

		public:
			static IStreamBasedDWriteFontFileLoader& GetInstance() {
				static IStreamBasedDWriteFontFileLoader s_instance;
				return s_instance;
			}

			HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override {
				if (riid == __uuidof(IUnknown))
					*ppvObject = static_cast<IUnknown*>(this);
				else if (riid == __uuidof(IDWriteFontFileLoader))
					*ppvObject = static_cast<IDWriteFontFileLoader*>(this);
				else
					*ppvObject = nullptr;

				if (!*ppvObject)
					return E_NOINTERFACE;

				AddRef();
				return S_OK;
			}

			ULONG __stdcall AddRef(void) override {
				return 1;
			}

			ULONG __stdcall Release(void) override {
				return 0;
			}

			HRESULT __stdcall CreateStreamFromKey(void const* fontFileReferenceKey, uint32_t fontFileReferenceKeySize, IDWriteFontFileStream** pFontFileStream) {
				if (fontFileReferenceKeySize != sizeof(std::shared_ptr<IStream>))
					return E_INVALIDARG;

				*pFontFileStream = IStreamAsDWriteFontFileStream::New(*static_cast<const std::shared_ptr<IStream>*>(fontFileReferenceKey));
				return S_OK;
			}
		};

		class IStreamBasedDWriteFontCollectionLoader : public IDWriteFontCollectionLoader {
		public:
			class IStreamAsDWriteFontFileEnumerator : public IDWriteFontFileEnumerator {
				const IDWriteFactoryPtr m_factory;
				const std::shared_ptr<IStream> m_stream;

				std::atomic_uint32_t m_nRef = 1;
				int m_nCurrentFile = -1;

				IStreamAsDWriteFontFileEnumerator(IDWriteFactoryPtr factoryPtr, std::shared_ptr<IStream> pStream)
					: m_factory(factoryPtr)
					, m_stream(std::move(pStream)) {}

			public:
				static IStreamAsDWriteFontFileEnumerator* New(IDWriteFactoryPtr factoryPtr, std::shared_ptr<IStream> pStream) {
					return new IStreamAsDWriteFontFileEnumerator(std::move(factoryPtr), std::move(pStream));
				}

				HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override {
					if (riid == __uuidof(IUnknown))
						*ppvObject = static_cast<IUnknown*>(this);
					else if (riid == __uuidof(IDWriteFontFileEnumerator))
						*ppvObject = static_cast<IDWriteFontFileEnumerator*>(this);
					else
						*ppvObject = nullptr;

					if (!*ppvObject)
						return E_NOINTERFACE;

					AddRef();
					return S_OK;
				}

				ULONG __stdcall AddRef(void) override {
					return ++m_nRef;
				}

				ULONG __stdcall Release(void) override {
					const auto newRef = --m_nRef;
					if (!newRef)
						delete this;
					return newRef;
				}

				HRESULT __stdcall MoveNext(BOOL* pHasCurrentFile) override {
					if (m_nCurrentFile == -1) {
						m_nCurrentFile = 0;
						*pHasCurrentFile = TRUE;
					} else {
						m_nCurrentFile = 1;
						*pHasCurrentFile = FALSE;
					}
					return S_OK;
				}

				HRESULT __stdcall GetCurrentFontFile(IDWriteFontFile** pFontFile) override {
					if (m_nCurrentFile != 0)
						return E_FAIL;

					return m_factory->CreateCustomFontFileReference(&m_stream, sizeof m_stream, &IStreamBasedDWriteFontFileLoader::GetInstance(), pFontFile);
				}
			};

			IStreamBasedDWriteFontCollectionLoader() = default;
			IStreamBasedDWriteFontCollectionLoader(IStreamBasedDWriteFontCollectionLoader&&) = delete;
			IStreamBasedDWriteFontCollectionLoader(const IStreamBasedDWriteFontCollectionLoader&) = delete;
			IStreamBasedDWriteFontCollectionLoader& operator=(IStreamBasedDWriteFontCollectionLoader&&) = delete;
			IStreamBasedDWriteFontCollectionLoader& operator=(const IStreamBasedDWriteFontCollectionLoader&) = delete;

		public:
			static IStreamBasedDWriteFontCollectionLoader& GetInstance() {
				static IStreamBasedDWriteFontCollectionLoader s_instance;
				return s_instance;
			}

			HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObject) override {
				if (riid == __uuidof(IUnknown))
					*ppvObject = static_cast<IUnknown*>(this);
				else if (riid == __uuidof(IDWriteFontCollectionLoader))
					*ppvObject = static_cast<IDWriteFontCollectionLoader*>(this);
				else
					*ppvObject = nullptr;

				if (!*ppvObject)
					return E_NOINTERFACE;

				AddRef();
				return S_OK;
			}

			ULONG __stdcall AddRef(void) override {
				return 1;
			}

			ULONG __stdcall Release(void) override {
				return 0;
			}

			HRESULT __stdcall CreateEnumeratorFromKey(IDWriteFactory* factory, void const* collectionKey, uint32_t collectionKeySize, IDWriteFontFileEnumerator** pFontFileEnumerator) {
				if (collectionKeySize != sizeof(std::shared_ptr<IStream>))
					return E_INVALIDARG;


				*pFontFileEnumerator = IStreamAsDWriteFontFileEnumerator::New(IDWriteFactoryPtr(factory, true), *static_cast<const std::shared_ptr<IStream>*>(collectionKey));
				return S_OK;
			}
		};

		class DWriteFontTable {
			const IDWriteFontFacePtr m_pFontFace;
			const void* m_pData;
			void* m_pTableContext;
			uint32_t m_nSize;
			BOOL m_bExists;

		public:
			DWriteFontTable(IDWriteFontFace* pFace, uint32_t tag)
				: m_pFontFace(pFace, true) {
				SuccessOrThrow(pFace->TryGetFontTable(tag, &m_pData, &m_nSize, &m_pTableContext, &m_bExists));
			}

			~DWriteFontTable() {
				if (m_bExists)
					m_pFontFace->ReleaseFontTable(m_pTableContext);
			}

			operator bool() const {
				return m_bExists;
			}

			template<typename T = uint8_t>
			std::span<const T> GetSpan() const {
				if (!m_bExists)
					return {};

				return { static_cast<const T*>(m_pData), m_nSize };
			}
		};

	public:
		struct CreateStruct {
			DWRITE_RENDERING_MODE RenderMode = DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL;
			DWRITE_MEASURING_MODE MeasureMode = DWRITE_MEASURING_MODE_GDI_CLASSIC;
			DWRITE_GRID_FIT_MODE GridFitMode = DWRITE_GRID_FIT_MODE_ENABLED;

			const wchar_t* GetMeasuringModeString() const {
				switch (MeasureMode) {
					case DWRITE_MEASURING_MODE_NATURAL: return L"Natural";
					case DWRITE_MEASURING_MODE_GDI_CLASSIC: return L"GDI Classic";
					case DWRITE_MEASURING_MODE_GDI_NATURAL: return L"GDI Natural";
					default: return L"Invalid";
				}
			}

			const wchar_t* GetRenderModeString() const {
				switch (RenderMode) {
					case DWRITE_RENDERING_MODE_DEFAULT: return L"Default";
					case DWRITE_RENDERING_MODE_ALIASED: return L"Aliased";
					case DWRITE_RENDERING_MODE_GDI_CLASSIC: return L"GDI Classic";
					case DWRITE_RENDERING_MODE_GDI_NATURAL: return L"GDI Natural";
					case DWRITE_RENDERING_MODE_NATURAL: return L"Natural";
					case DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC: return L"Natural Symmetric";
					default: return L"Invalid";
				}
			}

			const wchar_t* GetGridFitModeString() const {
				switch (GridFitMode) {
					case DWRITE_GRID_FIT_MODE_DEFAULT: return L"Default";
					case DWRITE_GRID_FIT_MODE_DISABLED: return L"Disabled";
					case DWRITE_GRID_FIT_MODE_ENABLED: return L"Enabled";
					default: return L"Invalid";
				}
			}
		};

	private:
		struct ParsedInfoStruct {
			std::shared_ptr<IStream> Stream;
			std::set<char32_t> Characters;
			std::map<std::pair<char32_t, char32_t>, int> KerningPairs;
			std::map<char32_t, GlyphMetrics> AllGlyphMetrics;
			DWRITE_FONT_METRICS1 Metrics;
			CreateStruct Params;
			int FontIndex = 0;
			float Size = 0.f;
			int RecommendedHorizontalOffset = 0;
			int MaximumRequiredHorizontalOffset = 0;

			template<decltype(std::roundf) TIntCastFn = std::roundf>
			int ScaleFromFontUnit(int fontUnitValue) const {
				return static_cast<int>(TIntCastFn(static_cast<float>(fontUnitValue) * Size / static_cast<float>(Metrics.designUnitsPerEm)));
			}
		};

		struct DWriteInterfaceStruct {
			IDWriteFactoryPtr Factory;
			IDWriteFactory3Ptr Factory3;
			IDWriteFontCollectionPtr Collection;
			IDWriteFontFamilyPtr Family;
			IDWriteFontPtr Font;
			IDWriteFontFacePtr Face;
			IDWriteFontFace1Ptr Face1;
		};

		DWriteInterfaceStruct m_dwrite;
		std::shared_ptr<const ParsedInfoStruct> m_info;
		mutable std::vector<uint8_t> m_drawBuffer;

	public:
		DirectWriteFixedSizeFont(std::filesystem::path path, int fontIndex, float size, CreateStruct params)
			: DirectWriteFixedSizeFont(std::make_shared<MemoryStream>(FileStream(path)), fontIndex, size, std::move(params)) {}

		DirectWriteFixedSizeFont(std::shared_ptr<IStream> stream, int fontIndex, float size, CreateStruct params) {
			if (!stream)
				return;

			auto info = std::make_shared<ParsedInfoStruct>();
			info->Stream = std::move(stream);
			info->Params = std::move(params);
			info->FontIndex = fontIndex;
			info->Size = size;

			m_dwrite = FaceFromInfoStruct(*info);
			m_dwrite.Face->GetMetrics(&info->Metrics);

			{
				uint32_t rangeCount;
				SuccessOrThrow(m_dwrite.Face1->GetUnicodeRanges(0, nullptr, &rangeCount), { E_NOT_SUFFICIENT_BUFFER });
				std::vector<DWRITE_UNICODE_RANGE> ranges(rangeCount);
				SuccessOrThrow(m_dwrite.Face1->GetUnicodeRanges(rangeCount, &ranges[0], &rangeCount));

				for (const auto& range : ranges)
					for (uint32_t i = range.first; i <= range.last; ++i)
						info->Characters.insert(static_cast<char32_t>(i));
			}
			{
				const auto ascent = info->ScaleFromFontUnit(info->Metrics.ascent);
				std::vector<uint64_t> horzOffsets;
				horzOffsets.reserve(info->Characters.size());
				for (const auto c : info->Characters) {
					IDWriteGlyphRunAnalysisPtr analysis;
					auto& gm = info->AllGlyphMetrics[c];
					if (!GetGlyphMetrics(m_dwrite, *info, c, gm, analysis))
						continue;

					gm.Translate(0, ascent);

					const auto n = gm.X1;
					if (n <= 0)
						continue;
					if (n >= horzOffsets.size())
						horzOffsets.resize(n + static_cast<size_t>(1));
					horzOffsets[n]++;
					horzOffsets.push_back(n);
					info->MaximumRequiredHorizontalOffset = (std::max)(info->MaximumRequiredHorizontalOffset, n);

					info->RecommendedHorizontalOffset = static_cast<int>(std::accumulate(horzOffsets.begin(), horzOffsets.end(), 0ULL) / horzOffsets.size());
				}
			}

			{
				DWriteFontTable kernDataRef(m_dwrite.Face, Internal::TrueType::Kern::DirectoryTableTag.NativeValue);
				DWriteFontTable gposDataRef(m_dwrite.Face, Internal::TrueType::Gpos::DirectoryTableTag.NativeValue);
				DWriteFontTable cmapDataRef(m_dwrite.Face, Internal::TrueType::Cmap::DirectoryTableTag.NativeValue);
				Internal::TrueType::Kern::View kern(kernDataRef.GetSpan<char>());
				Internal::TrueType::Gpos::View gpos(gposDataRef.GetSpan<char>());
				Internal::TrueType::Cmap::View cmap(cmapDataRef.GetSpan<char>());
				if (cmap && (kern || gpos)) {
					const auto cmapVector = cmap.GetGlyphToCharMap();

					if (kern)
						info->KerningPairs = kern.Parse(cmapVector);

					if (gpos) {
						const auto pairs = gpos.ExtractAdvanceX(cmapVector);
						// do not overwrite
						info->KerningPairs.insert(pairs.begin(), pairs.end());
					}

					for (auto it = info->KerningPairs.begin(); it != info->KerningPairs.end(); ) {
						it->second = info->ScaleFromFontUnit(it->second);
						if (it->second)
							++it;
						else
							it = info->KerningPairs.erase(it);
					}
				}
			}

			m_info = std::move(info);
		}

		DirectWriteFixedSizeFont() = default;
		DirectWriteFixedSizeFont(DirectWriteFixedSizeFont&&) = default;
		DirectWriteFixedSizeFont& operator=(DirectWriteFixedSizeFont&&) = default;

		DirectWriteFixedSizeFont(const DirectWriteFixedSizeFont& r) : DirectWriteFixedSizeFont() {
			if (r.m_info == nullptr)
				return;

			m_dwrite = FaceFromInfoStruct(*r.m_info);
			m_info = r.m_info;
		}

		DirectWriteFixedSizeFont& operator=(const DirectWriteFixedSizeFont& r) {
			if (this == &r)
				return *this;

			if (r.m_info == nullptr) {
				m_dwrite = {};
				m_info = nullptr;
			} else {
				m_dwrite = FaceFromInfoStruct(*r.m_info);
				m_info = r.m_info;
			}

			return *this;
		}

		std::string GetFamilyName() const override {
			IDWriteLocalizedStringsPtr strings;
			SuccessOrThrow(m_dwrite.Family->GetFamilyNames(&strings));

			uint32_t index;
			BOOL exists;
			SuccessOrThrow(strings->FindLocaleName(L"en-us", &index, &exists));
			if (exists)
				index = 0;

			uint32_t length;
			SuccessOrThrow(strings->GetStringLength(index, &length));

			std::wstring res(length + 1, L'\0');
			SuccessOrThrow(strings->GetString(index, &res[0], length + 1));
			res.resize(length);

			return Unicode::Convert<std::string>(res);
		}

		std::string GetSubfamilyName() const override {
			IDWriteLocalizedStringsPtr strings;
			SuccessOrThrow(m_dwrite.Font->GetFaceNames(&strings));

			uint32_t index;
			BOOL exists;
			SuccessOrThrow(strings->FindLocaleName(L"en-us", &index, &exists));
			if (exists)
				index = 0;

			uint32_t length;
			SuccessOrThrow(strings->GetStringLength(index, &length));

			std::wstring res(length + 1, L'\0');
			SuccessOrThrow(strings->GetString(index, &res[0], length + 1));
			res.resize(length);

			return Unicode::Convert<std::string>(res);
		}

		int GetRecommendedHorizontalOffset() const override {
			return m_info->RecommendedHorizontalOffset;
		}

		int GetMaximumRequiredHorizontalOffset() const override {
			return m_info->MaximumRequiredHorizontalOffset;
		}

		float GetSize() const override {
			return m_info->Size;
		}

		int GetAscent() const override {
			return m_info->ScaleFromFontUnit(m_info->Metrics.ascent);
		}

		int GetLineHeight() const override {
			return m_info->ScaleFromFontUnit(m_info->Metrics.ascent + m_info->Metrics.descent + m_info->Metrics.lineGap);
		}

		const std::set<char32_t>& GetAllCodepoints() const override {
			return m_info->Characters;
		}

		const std::map<char32_t, GlyphMetrics>& GetAllGlyphMetrics() const override {
			return m_info->AllGlyphMetrics;
		}

		const std::map<std::pair<char32_t, char32_t>, int>& GetAllKerningPairs() const override {
			return m_info->KerningPairs;
		}

		bool Draw(char32_t codepoint, RGBA8888* pBuf, int drawX, int drawY, int destWidth, int destHeight, RGBA8888 fgColor, RGBA8888 bgColor, float gamma) const override {
			IDWriteGlyphRunAnalysisPtr analysis;
			GlyphMetrics gm;
			if (!GetGlyphMetrics(m_dwrite, *m_info, codepoint, gm, analysis))
				return false;

			auto src = gm;
			src.Translate(-src.X1, -src.Y1);
			auto dest = gm;
			dest.Translate(drawX, drawY + GetAscent());
			src.AdjustToIntersection(dest, src.GetWidth(), src.GetHeight(), destWidth, destHeight);
			if (src.IsEffectivelyEmpty() || dest.IsEffectivelyEmpty())
				return true;

			m_drawBuffer.resize(gm.GetArea());
			SuccessOrThrow(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, gm.AsConstRectPtr(), &m_drawBuffer[0], static_cast<uint32_t>(m_drawBuffer.size())));

			Internal::BitmapCopy::ToRGBA8888()
				.From(&m_drawBuffer[0], gm.GetWidth(), gm.GetHeight(), 1, Internal::BitmapVerticalDirection::TopRowFirst)
				.To(pBuf, destWidth, destHeight, Internal::BitmapVerticalDirection::TopRowFirst)
				.WithForegroundColor(fgColor)
				.WithBackgroundColor(bgColor)
				.WithGamma(gamma)
				.CopyTo(src.X1, src.Y1, src.X2, src.Y2, dest.X1, dest.Y1);

			return true;
		}

		bool Draw(char32_t codepoint, uint8_t* pBuf, size_t stride, int drawX, int drawY, int destWidth, int destHeight, uint8_t fgColor, uint8_t bgColor, uint8_t fgOpacity, uint8_t bgOpacity, float gamma) const override {
			IDWriteGlyphRunAnalysisPtr analysis;
			GlyphMetrics gm;
			if (!GetGlyphMetrics(m_dwrite, *m_info, codepoint, gm, analysis))
				return false;

			auto src = gm;
			src.Translate(-src.X1, -src.Y1);
			auto dest = gm;
			dest.Translate(drawX, drawY + GetAscent());
			src.AdjustToIntersection(dest, src.GetWidth(), src.GetHeight(), destWidth, destHeight);
			if (src.IsEffectivelyEmpty() || dest.IsEffectivelyEmpty())
				return true;

			m_drawBuffer.resize(gm.GetArea());
			SuccessOrThrow(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, gm.AsConstRectPtr(), &m_drawBuffer[0], static_cast<uint32_t>(m_drawBuffer.size())));

			Internal::BitmapCopy::ToL8()
				.From(&m_drawBuffer[0], gm.GetWidth(), gm.GetHeight(), 1, Internal::BitmapVerticalDirection::TopRowFirst)
				.To(pBuf, destWidth, destHeight, stride, Internal::BitmapVerticalDirection::TopRowFirst)
				.WithForegroundColor(fgColor)
				.WithForegroundOpacity(fgOpacity)
				.WithBackgroundColor(bgColor)
				.WithBackgroundOpacity(bgOpacity)
				.WithGamma(gamma)
				.CopyTo(src.X1, src.Y1, src.X2, src.Y2, dest.X1, dest.Y1);
			return true;
		}

		std::shared_ptr<IFixedSizeFont> GetThreadSafeView() const override {
			return std::make_shared<DirectWriteFixedSizeFont>(*this);
		}

	private:
		static DWriteInterfaceStruct FaceFromInfoStruct(const ParsedInfoStruct& info) {
			DWriteInterfaceStruct res{};

			SuccessOrThrow(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&res.Factory)));
			SuccessOrThrow(res.Factory->RegisterFontFileLoader(&IStreamBasedDWriteFontFileLoader::GetInstance()), { DWRITE_E_ALREADYREGISTERED });
			SuccessOrThrow(res.Factory->RegisterFontCollectionLoader(&IStreamBasedDWriteFontCollectionLoader::GetInstance()), { DWRITE_E_ALREADYREGISTERED });
			SuccessOrThrow(res.Factory.QueryInterface(decltype(res.Factory3)::GetIID(), &res.Factory3), { E_NOINTERFACE });
			SuccessOrThrow(res.Factory->CreateCustomFontCollection(&IStreamBasedDWriteFontCollectionLoader::GetInstance(), &info.Stream, sizeof info.Stream, &res.Collection));
			SuccessOrThrow(res.Collection->GetFontFamily(0, &res.Family));
			SuccessOrThrow(res.Family->GetFont(info.FontIndex, &res.Font));
			SuccessOrThrow(res.Font->CreateFontFace(&res.Face));
			SuccessOrThrow(res.Face.QueryInterface(decltype(res.Face1)::GetIID(), &res.Face1));

			return res;
		}

		static bool GetGlyphMetrics(const DWriteInterfaceStruct& iface, const ParsedInfoStruct& info, char32_t codepoint, GlyphMetrics& gm, IDWriteGlyphRunAnalysisPtr& analysis) {
			try {
				uint16_t glyphIndex;
				SuccessOrThrow(iface.Face->GetGlyphIndices(reinterpret_cast<const uint32_t*>(&codepoint), 1, &glyphIndex));
				if (!glyphIndex) {
					gm.Clear();
					return false;
				}

				DWRITE_GLYPH_METRICS dgm;
				SuccessOrThrow(iface.Face->GetGdiCompatibleGlyphMetrics(
					info.Size, 1.0f, nullptr,
					info.Params.MeasureMode == DWRITE_MEASURING_MODE_GDI_NATURAL ? TRUE : FALSE,
					&glyphIndex, 1, &dgm));

				float glyphAdvance{};
				DWRITE_GLYPH_OFFSET glyphOffset{};
				const DWRITE_GLYPH_RUN run{
					.fontFace = iface.Face,
					.fontEmSize = info.Size,
					.glyphCount = 1,
					.glyphIndices = &glyphIndex,
					.glyphAdvances = &glyphAdvance,
					.glyphOffsets = &glyphOffset,
					.isSideways = FALSE,
					.bidiLevel = 0,
				};

				auto renderMode = info.Params.RenderMode;
				if (renderMode == DWRITE_RENDERING_MODE_DEFAULT)
					SuccessOrThrow(iface.Face->GetRecommendedRenderingMode(info.Size, 1.f, info.Params.MeasureMode, nullptr, &renderMode));
				
				SuccessOrThrow(iface.Factory3->CreateGlyphRunAnalysis(
					&run,
					nullptr,
					renderMode,
					info.Params.MeasureMode,
					info.Params.GridFitMode,
					DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
					0,
					0,
					&analysis));
				
				SuccessOrThrow(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, gm.AsMutableRectPtr()));
				
				gm.AdvanceX = info.ScaleFromFontUnit(dgm.advanceWidth);

				return true;

			} catch (...) {
				gm.Clear();
				return false;
			}
		}
	};
}

#endif
