#ifndef _XIVRES_FONTGENERATOR_MERGEDFIXEDSIZEFONT_H_
#define _XIVRES_FONTGENERATOR_MERGEDFIXEDSIZEFONT_H_

#include "IFixedSizeFont.h"

namespace XivRes::FontGenerator {
	enum class MergedFontVerticalAlignment {
		Top,
		Middle,
		Baseline,
		Bottom,
	};

	class MergedFixedSizeFont : public DefaultAbstractFixedSizeFont {
		struct InfoStruct {
			std::set<char32_t> Codepoints;
			std::map<std::pair<char32_t, char32_t>, int> KerningPairs;
			std::map<char32_t, GlyphMetrics> AllGlyphMetrics;
			std::map<char32_t, IFixedSizeFont*> UsedFonts;
			float Size{};
			int Ascent{};
			int LineHeight{};
			MergedFontVerticalAlignment Alignment = MergedFontVerticalAlignment::Baseline;
		};

		std::vector<std::shared_ptr<XivRes::FontGenerator::IFixedSizeFont>> m_fonts;
		std::shared_ptr<const InfoStruct> m_info;

	public:
		MergedFixedSizeFont() = default;
		MergedFixedSizeFont(MergedFixedSizeFont&&) = default;
		MergedFixedSizeFont(const MergedFixedSizeFont&) = default;
		MergedFixedSizeFont& operator=(MergedFixedSizeFont&&) = default;
		MergedFixedSizeFont& operator=(const MergedFixedSizeFont&) = default;

		MergedFixedSizeFont(std::vector<std::pair<std::shared_ptr<IFixedSizeFont>, bool>> fonts, MergedFontVerticalAlignment verticalAlignment = MergedFontVerticalAlignment::Baseline) {
			if (fonts.empty())
				return;

			auto info = std::make_shared<InfoStruct>();
			info->Size = fonts.front().first->GetSize();
			info->Ascent = fonts.front().first->GetAscent();
			info->LineHeight = fonts.front().first->GetLineHeight();
			info->Alignment = verticalAlignment;

			for (auto& [font, overwrite] : fonts) {
				for (const auto c : font->GetAllCodepoints()) {
					if (overwrite) {
						info->UsedFonts.insert_or_assign(c, font.get());
						info->Codepoints.insert(c);
					} else if (info->UsedFonts.emplace(c, font.get()).second)
						info->Codepoints.insert(c);
				}

				m_fonts.emplace_back(std::move(font));
			}

			std::map<IFixedSizeFont*, std::set<char32_t>> charsPerFonts;
			for (const auto& [c, f] : info->UsedFonts)
				charsPerFonts[f].insert(c);

			for (const auto& [font, chars] : charsPerFonts) {
				for (const auto& kerningPair : font->GetAllKerningPairs()) {
					if (chars.contains(kerningPair.first.first) && chars.contains(kerningPair.first.second))
						info->KerningPairs.emplace(kerningPair);
				}

				for (const auto& c : chars) {
					auto& gm = info->AllGlyphMetrics[c];
					if (font->GetGlyphMetrics(c, gm))
						gm.Translate(0, GetVerticalAdjustment(*info, *font));
				}
			}

			m_info = std::move(info);
		}

		MergedFontVerticalAlignment GetComponentVerticalAlignment() const {
			return m_info->Alignment;
		}

		std::string GetFamilyName() const override {
			return "Merged";
		}

		std::string GetSubfamilyName() const override {
			return {};
		}

		int GetRecommendedHorizontalOffset() const override {
			return 0;
		}

		int GetMaximumRequiredHorizontalOffset() const override {
			return 0;
		}

		float GetSize() const override {
			return m_info->Size;
		}

		int GetAscent() const override {
			return m_info->Ascent;
		}

		int GetLineHeight() const override {
			return m_info->LineHeight;
		}

		const std::set<char32_t>& GetAllCodepoints() const override {
			return m_info->Codepoints;
		}

		const std::map<char32_t, GlyphMetrics>& GetAllGlyphMetrics() const {
			return m_info->AllGlyphMetrics;
		}

		const void* GetGlyphUniqid(char32_t c) const override {
			if (const auto it = m_info->UsedFonts.find(c); it != m_info->UsedFonts.end())
				return it->second->GetGlyphUniqid(c);

			return nullptr;
		}

		const std::map<std::pair<char32_t, char32_t>, int>& GetAllKerningPairs() const override {
			return m_info->KerningPairs;
		}

		bool Draw(char32_t codepoint, RGBA8888* pBuf, int drawX, int drawY, int destWidth, int destHeight, RGBA8888 fgColor, RGBA8888 bgColor, float gamma) const override {
			if (const auto it = m_info->UsedFonts.find(codepoint); it != m_info->UsedFonts.end())
				return it->second->Draw(codepoint, pBuf, drawX, drawY + GetVerticalAdjustment(*m_info, *it->second), destWidth, destHeight, fgColor, bgColor, gamma);

			return false;
		}

		bool Draw(char32_t codepoint, uint8_t* pBuf, size_t stride, int drawX, int drawY, int destWidth, int destHeight, uint8_t fgColor, uint8_t bgColor, uint8_t fgOpacity, uint8_t bgOpacity, float gamma) const override {
			if (const auto it = m_info->UsedFonts.find(codepoint); it != m_info->UsedFonts.end())
				return it->second->Draw(codepoint, pBuf, stride, drawX, drawY + GetVerticalAdjustment(*m_info, *it->second), destWidth, destHeight, fgColor, bgColor, fgOpacity, bgOpacity, gamma);

			return false;
		}

		std::shared_ptr<IFixedSizeFont> GetThreadSafeView() const override {
			auto res = std::make_shared<MergedFixedSizeFont>(*this);
			res->m_fonts.clear();
			for (const auto& font : m_fonts)
				res->m_fonts.emplace_back(font->GetThreadSafeView());
			return res;
		}

	private:
		static int GetVerticalAdjustment(const InfoStruct& info, const XivRes::FontGenerator::IFixedSizeFont& font) {
			switch (info.Alignment) {
				case MergedFontVerticalAlignment::Top:
					return 0;
				case MergedFontVerticalAlignment::Middle:
					return 0 + (info.LineHeight - font.GetLineHeight()) / 2;
				case MergedFontVerticalAlignment::Baseline:
					return 0 + info.Ascent - font.GetAscent();
				case MergedFontVerticalAlignment::Bottom:
					return 0 + info.LineHeight - font.GetLineHeight();
				default:
					throw std::runtime_error("Invalid alignment value set");
			}
		}
	};
}

#endif
