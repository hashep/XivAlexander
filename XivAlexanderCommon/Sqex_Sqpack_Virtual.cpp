#include "pch.h"
#include "Sqex_Sqpack_Virtual.h"

#include <fstream>

#include "Sqex_Model.h"
#include "Sqex_Sqpack_EntryProvider.h"
#include "Sqex_Sqpack_Reader.h"
#include "Sqex_ThirdParty_TexTools.h"

struct Sqex::Sqpack::VirtualSqPack::Implementation {
	AddEntryResult AddEntry(std::shared_ptr<EntryProvider> provider, bool overwriteExisting = true);

	struct Entry {
		uint32_t DataFileIndex;
		uint32_t BlockSize;
		uint32_t PadSize;
		SqIndex::LEDataLocator Locator;

		uint64_t OffsetAfterHeaders;
		std::shared_ptr<EntryProvider> Provider;
	};

	VirtualSqPack* const this_;

	std::vector<std::unique_ptr<Entry>> m_entries;
	std::map<std::pair<uint32_t, uint32_t>, Entry*> m_pathNameTupleEntryPointerMap;
	std::map<uint32_t, Entry*> m_fullPathEntryPointerMap;

	std::vector<SqIndex::FileSegmentEntry> m_fileEntries1;
	std::vector<SqIndex::FileSegmentEntry2> m_fileEntries2;
	std::vector<SqIndex::FolderSegmentEntry> m_folderEntries;

	bool m_frozen = false;

	std::vector<Utils::Win32::File> m_openFiles;

	Implementation(VirtualSqPack* this_)
		: this_(this_) {
	}

	virtual ~Implementation() = default;

	template<typename...Args>
	void Log(Args...args) {
		if (this_->Log.Empty())
			return;

		this_->Log(std::format(std::forward<Args>(args)...));
	}

	SqData::Header& AllocateDataSpace(size_t length, bool strict) {
		if (this_->m_sqpackDataSubHeaders.empty() ||
			sizeof SqpackHeader + sizeof SqData::Header + this_->m_sqpackDataSubHeaders.back().DataSize + length > this_->m_sqpackDataSubHeaders.back().MaxFileSize) {
			if (strict && !this_->m_sqpackDataSubHeaders.empty())
				this_->m_sqpackDataSubHeaders.back().Sha1.SetFromSpan(&this_->m_sqpackDataSubHeaders.back(), 1);
			this_->m_sqpackDataSubHeaders.emplace_back(SqData::Header{
				.HeaderSize = sizeof SqData::Header,
				.Unknown1 = SqData::Header::Unknown1_Value,
				.DataSize = 0,
				.SpanIndex = static_cast<uint32_t>(this_->m_sqpackDataSubHeaders.size()),
				.MaxFileSize = this_->m_maxFileSize,
				});
		}
		return this_->m_sqpackDataSubHeaders.back();
	}
};

Sqex::Sqpack::VirtualSqPack::VirtualSqPack(std::string ex, std::string name, uint64_t maxFileSize)
	: m_maxFileSize(maxFileSize)
	, DatExpac(std::move(ex))
	, DatName(std::move(name))
	, m_pImpl(std::make_unique<Implementation>(this)) {
	if (maxFileSize > SqData::Header::MaxFileSize_MaxValue)
		throw std::invalid_argument("MaxFileSize cannot be more than 32GiB.");
}

Sqex::Sqpack::VirtualSqPack::~VirtualSqPack() = default;

Utils::Win32::File Sqex::Sqpack::VirtualSqPack::OpenFile(
	_In_opt_ std::filesystem::path curItemPath,
	_In_opt_ Utils::Win32::File alreadyOpenedFile
) {
	if (curItemPath.empty()) {
		if (!alreadyOpenedFile)
			throw std::invalid_argument("curItemPath and alreadyOpenedFile cannot both be empty");
		else
			curItemPath = alreadyOpenedFile.ResolveName();
	}

	size_t found;
	for (found = 0; found < m_pImpl->m_openFiles.size(); ++found) {
		if (equivalent(m_pImpl->m_openFiles[found].ResolveName(), curItemPath)) {
			break;
		}
	}
	if (found == m_pImpl->m_openFiles.size()) {
		if (alreadyOpenedFile) {
			if (!alreadyOpenedFile.HasOwnership())
				alreadyOpenedFile = Utils::Win32::File::DuplicateFrom<Utils::Win32::File>(alreadyOpenedFile);
		} else
			alreadyOpenedFile = Utils::Win32::File::Create(curItemPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0);

		m_pImpl->m_openFiles.emplace_back(std::move(alreadyOpenedFile));
	}

	return { m_pImpl->m_openFiles[found], false };
}

Sqex::Sqpack::VirtualSqPack::AddEntryResult& Sqex::Sqpack::VirtualSqPack::AddEntryResult::operator+=(const AddEntryResult & r) {
	auto& k = r.Added;
	Added.insert(Added.end(), r.Added.begin(), r.Added.end());
	Replaced.insert(Replaced.end(), r.Replaced.begin(), r.Replaced.end());
	SkippedExisting.insert(SkippedExisting.end(), r.SkippedExisting.begin(), r.SkippedExisting.end());
	return *this;
}

Sqex::Sqpack::EntryProvider* Sqex::Sqpack::VirtualSqPack::AddEntryResult::AnyItem() const {
	if (!Added.empty())
		return Added[0];
	if (!Replaced.empty())
		return Replaced[0];
	if (!SkippedExisting.empty())
		return SkippedExisting[0];
	return nullptr;
}

std::vector<Sqex::Sqpack::EntryProvider*> Sqex::Sqpack::VirtualSqPack::AddEntryResult::AllEntries() const {
	std::vector<EntryProvider*> res;
	res.insert(res.end(), Added.begin(), Added.end());
	res.insert(res.end(), Replaced.begin(), Replaced.end());
	res.insert(res.end(), SkippedExisting.begin(), SkippedExisting.end());
	return res;
}

Sqex::Sqpack::VirtualSqPack::AddEntryResult Sqex::Sqpack::VirtualSqPack::Implementation::AddEntry(std::shared_ptr<EntryProvider> provider, bool overwriteExisting) {
	if (m_frozen)
		throw std::runtime_error("Trying to operate on a frozen VirtualSqPack");

	if (provider->PathSpec().HasComponentHash()) {
		const auto it = m_pathNameTupleEntryPointerMap.find(std::make_pair(provider->PathSpec().PathHash, provider->PathSpec().NameHash));
		if (it != m_pathNameTupleEntryPointerMap.end()) {
			if (!overwriteExisting) {
				it->second->Provider->UpdatePathSpec(provider->PathSpec());
				return { .SkippedExisting = {it->second->Provider.get()} };
			}
			it->second->Provider = std::move(provider);
			return { .Replaced = {it->second->Provider.get()} };
		}
	}
	if (provider->PathSpec().FullPathHash != EntryPathSpec::EmptyHashValue) {
		const auto it = m_fullPathEntryPointerMap.find(provider->PathSpec().FullPathHash);
		if (it != m_fullPathEntryPointerMap.end()) {
			if (!overwriteExisting) {
				it->second->Provider->UpdatePathSpec(provider->PathSpec());
				return { .SkippedExisting = {it->second->Provider.get()} };
			}
			it->second->Provider = std::move(provider);
			return { .Replaced = {it->second->Provider.get()} };
		}
	}

	const auto pProvider = provider.get();
	auto entry = std::make_unique<Entry>(0, 0, 0, 0, SqIndex::LEDataLocator{ 0, 0 }, std::move(provider));
	if (entry->Provider->PathSpec().HasFullPathHash())
		m_fullPathEntryPointerMap.insert_or_assign(entry->Provider->PathSpec().FullPathHash, entry.get());
	if (entry->Provider->PathSpec().HasComponentHash())
		m_pathNameTupleEntryPointerMap.insert_or_assign(std::make_pair(entry->Provider->PathSpec().PathHash, entry->Provider->PathSpec().NameHash), entry.get());
	m_entries.emplace_back(std::move(entry));
	return { .Added = {pProvider} };
}

Sqex::Sqpack::VirtualSqPack::AddEntryResult Sqex::Sqpack::VirtualSqPack::AddEntriesFromSqPack(const std::filesystem::path & indexPath, bool overwriteExisting, bool overwriteUnknownSegments) {
	if (m_pImpl->m_frozen)
		throw std::runtime_error("Trying to operate on a frozen VirtualSqPack");

	Reader m_original{ indexPath, false };

	AddEntryResult result{};

	if (overwriteUnknownSegments) {
		m_sqpackIndexSegment2 = std::move(m_original.Index.DataFileSegment);
		m_sqpackIndexSegment3 = std::move(m_original.Index.Segment3);
		m_sqpackIndex2Segment2 = std::move(m_original.Index2.DataFileSegment);
		m_sqpackIndex2Segment3 = std::move(m_original.Index2.Segment3);
	}

	std::vector<Utils::Win32::File> dataFiles;
	for (auto& f : m_original.Data) {
		dataFiles.emplace_back(OpenFile("", std::move(f.FileOnDisk)));
	}

	for (const auto& entry : m_original.Files) {
		result += m_pImpl->AddEntry(
			m_original.GetEntryProvider(entry, Utils::Win32::File{ dataFiles[entry.DataFileIndex], false }),
			overwriteExisting);
	}

	return result;
}

Sqex::Sqpack::VirtualSqPack::AddEntryResult Sqex::Sqpack::VirtualSqPack::AddEntryFromFile(EntryPathSpec pathSpec, const std::filesystem::path & path, bool overwriteExisting) {
	if (m_pImpl->m_frozen)
		throw std::runtime_error("Trying to operate on a frozen VirtualSqPack");

	std::shared_ptr<EntryProvider> provider;
	if (file_size(path) == 0) {
		provider = std::make_shared<EmptyEntryProvider>(std::move(pathSpec));
	} else if (path.extension() == ".tex") {
		provider = std::make_shared<OnTheFlyTextureEntryProvider>(std::move(pathSpec), path);
	} else if (path.extension() == ".mdl") {
		provider = std::make_shared<OnTheFlyModelEntryProvider>(std::move(pathSpec), path);
	} else {
		// provider = std::make_shared<MemoryBinaryEntryProvider>(std::move(pathSpec), path);
		provider = std::make_shared<OnTheFlyBinaryEntryProvider>(std::move(pathSpec), path);
	}
	return m_pImpl->AddEntry(provider, overwriteExisting);
}

Sqex::Sqpack::VirtualSqPack::AddEntryResult Sqex::Sqpack::VirtualSqPack::AddEntriesFromTTMP(const std::filesystem::path & extractedDir, bool overwriteExisting) {
	AddEntryResult addEntryResult{};
	nlohmann::json conf;
	const auto ttmpdPath = extractedDir / "TTMPD.mpd";
	Utils::Win32::File ttmpd;
	const auto ttmpl = ThirdParty::TexTools::TTMPL::FromStream(FileRandomAccessStream{ Utils::Win32::File::Create(
		extractedDir / "TTMPL.mpl", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0
	) });

	if (const auto configPath = extractedDir / "choices.json"; exists(configPath)) {
		m_pImpl->Log("Config file found");
		std::ifstream in(configPath);
		in >> conf;
	}
	for (size_t i = 0; i < ttmpl.SimpleModsList.size(); ++i) {
		const auto& entry = ttmpl.SimpleModsList[i];
		if (conf.is_array() && i < conf.size() && conf[i].is_boolean() && !conf[i].get<boolean>())
			continue;
		if (entry.DatFile != DatName)
			continue;

		if (!ttmpd)
			ttmpd = OpenFile(ttmpdPath);

		addEntryResult += m_pImpl->AddEntry(std::make_shared<RandomAccessStreamAsEntryProviderView>(
			entry.FullPath,
			std::make_shared<FileRandomAccessStream>(ttmpd, entry.ModOffset, entry.ModSize)
			), overwriteExisting);

		m_pImpl->Log("{}: {} (Name: {} > {})",
			!addEntryResult.Added.empty() ? "Added" : !addEntryResult.Replaced.empty() ? "Replaced" : "Ignored",
			entry.FullPath, ttmpl.Name, entry.Name
		);
	}
	for (size_t pageObjectIndex = 0; pageObjectIndex < ttmpl.ModPackPages.size(); ++pageObjectIndex) {
		const auto& modGroups = ttmpl.ModPackPages[pageObjectIndex].ModGroups;
		const auto pageConf = conf.is_array() && pageObjectIndex < conf.size() && conf[pageObjectIndex].is_array() ?
			conf[pageObjectIndex] :
			nlohmann::json::array();

		for (size_t modGroupIndex = 0; modGroupIndex < modGroups.size(); ++modGroupIndex) {
			const auto& modGroup = modGroups[modGroupIndex];
			const auto choice = modGroupIndex < pageConf.size() ? pageConf[modGroupIndex].get<int>() : 0;
			const auto& option = modGroup.OptionList[choice];

			for (const auto& entry : option.ModsJsons) {
				if (entry.DatFile != DatName)
					continue;

				if (!ttmpd)
					ttmpd = OpenFile(ttmpdPath);

				addEntryResult += m_pImpl->AddEntry(std::make_shared<RandomAccessStreamAsEntryProviderView>(
					entry.FullPath,
					std::make_shared<FileRandomAccessStream>(ttmpd, entry.ModOffset, entry.ModSize)
					), overwriteExisting);

				m_pImpl->Log("{}: {} (Name: {} > {}({}) > {}({}) > {})",
					!addEntryResult.Added.empty() ? "Added" : !addEntryResult.Replaced.empty() ? "Replaced" : "Ignored",
					entry.FullPath,
					ttmpl.Name,
					modGroup.GroupName, modGroupIndex,
					option.Name, choice,
					entry.Name
				);
			}
		}
	}
	return addEntryResult;
}

size_t Sqex::Sqpack::VirtualSqPack::NumOfDataFiles() const {
	return m_sqpackDataSubHeaders.size();
}

void Sqex::Sqpack::VirtualSqPack::Freeze(bool strict) {
	if (m_pImpl->m_frozen)
		throw std::runtime_error("Cannot freeze again");

	m_pImpl->m_fileEntries1.clear();
	m_pImpl->m_fileEntries2.clear();

	m_sqpackIndexSubHeader.DataFilesSegment.Count = 1;
	m_sqpackIndex2SubHeader.DataFilesSegment.Count = 1;

	for (const auto& entry : m_pImpl->m_entries) {
		entry->BlockSize = static_cast<uint32_t>(entry->Provider->StreamSize());
		entry->PadSize = Align(entry->BlockSize).Pad;

		auto& dataSubHeader = m_pImpl->AllocateDataSpace(0ULL + entry->BlockSize + entry->PadSize, strict);
		entry->DataFileIndex = static_cast<uint32_t>(m_sqpackDataSubHeaders.size() - 1);
		entry->OffsetAfterHeaders = dataSubHeader.DataSize;

		dataSubHeader.DataSize = dataSubHeader.DataSize + entry->BlockSize + entry->PadSize;

		const auto dataLocator = SqIndex::LEDataLocator{
			entry->DataFileIndex,
			sizeof SqpackHeader + sizeof SqData::Header + entry->OffsetAfterHeaders,
		};
		entry->Locator = dataLocator;
		if (entry->Provider->PathSpec().HasComponentHash())
			m_pImpl->m_fileEntries1.emplace_back(SqIndex::FileSegmentEntry{ entry->Provider->PathSpec().NameHash, entry->Provider->PathSpec().PathHash, dataLocator, 0 });
		if (entry->Provider->PathSpec().HasFullPathHash())
			m_pImpl->m_fileEntries2.emplace_back(SqIndex::FileSegmentEntry2{ entry->Provider->PathSpec().FullPathHash, dataLocator });
	}

	std::sort(m_pImpl->m_fileEntries1.begin(), m_pImpl->m_fileEntries1.end(), [](const SqIndex::FileSegmentEntry& l, const SqIndex::FileSegmentEntry& r) {
		if (l.PathHash == r.PathHash)
			return l.NameHash < r.NameHash;
		else
			return l.PathHash < r.PathHash;
	});
	std::sort(m_pImpl->m_fileEntries2.begin(), m_pImpl->m_fileEntries2.end(), [](const SqIndex::FileSegmentEntry2& l, const SqIndex::FileSegmentEntry2& r) {
		return l.FullPathHash < r.FullPathHash;
	});

	memcpy(m_sqpackIndexHeader.Signature, SqpackHeader::Signature_Value, sizeof SqpackHeader::Signature_Value);
	m_sqpackIndexHeader.HeaderSize = sizeof SqpackHeader;
	m_sqpackIndexHeader.Unknown1 = SqpackHeader::Unknown1_Value;
	m_sqpackIndexHeader.Type = SqpackType::SqIndex;
	m_sqpackIndexHeader.Unknown2 = SqpackHeader::Unknown2_Value;
	if (strict)
		m_sqpackIndexHeader.Sha1.SetFromSpan(&m_sqpackIndexHeader, 1);

	m_sqpackIndexSubHeader.HeaderSize = sizeof SqIndex::Header;
	m_sqpackIndexSubHeader.Type = SqIndex::Header::IndexType::Index;
	m_sqpackIndexSubHeader.FileSegment.Count = 1;
	m_sqpackIndexSubHeader.FileSegment.Offset = m_sqpackIndexHeader.HeaderSize + m_sqpackIndexSubHeader.HeaderSize;
	m_sqpackIndexSubHeader.FileSegment.Size = static_cast<uint32_t>(std::span(m_pImpl->m_fileEntries1).size_bytes());
	m_sqpackIndexSubHeader.DataFilesSegment.Count = static_cast<uint32_t>(m_sqpackDataSubHeaders.size());
	m_sqpackIndexSubHeader.DataFilesSegment.Offset = m_sqpackIndexSubHeader.FileSegment.Offset + m_sqpackIndexSubHeader.FileSegment.Size;
	m_sqpackIndexSubHeader.DataFilesSegment.Size = static_cast<uint32_t>(std::span(m_sqpackIndexSegment2).size_bytes());
	m_sqpackIndexSubHeader.UnknownSegment3.Count = 0;
	m_sqpackIndexSubHeader.UnknownSegment3.Offset = m_sqpackIndexSubHeader.DataFilesSegment.Offset + m_sqpackIndexSubHeader.DataFilesSegment.Size;
	m_sqpackIndexSubHeader.UnknownSegment3.Size = static_cast<uint32_t>(std::span(m_sqpackIndexSegment3).size_bytes());
	m_sqpackIndexSubHeader.FolderSegment.Count = 0;
	m_sqpackIndexSubHeader.FolderSegment.Offset = m_sqpackIndexSubHeader.UnknownSegment3.Offset + m_sqpackIndexSubHeader.UnknownSegment3.Size;
	for (size_t i = 0; i < m_pImpl->m_fileEntries1.size(); ++i) {
		const auto& entry = m_pImpl->m_fileEntries1[i];
		if (m_pImpl->m_folderEntries.empty() || m_pImpl->m_folderEntries.back().NameHash != entry.PathHash) {
			m_pImpl->m_folderEntries.emplace_back(
				entry.PathHash,
				static_cast<uint32_t>(m_sqpackIndexSubHeader.FileSegment.Offset + i * sizeof entry),
				static_cast<uint32_t>(sizeof entry),
				0);
		} else {
			m_pImpl->m_folderEntries.back().FileSegmentSize = m_pImpl->m_folderEntries.back().FileSegmentSize + sizeof entry;
		}
	}
	m_sqpackIndexSubHeader.FolderSegment.Size = static_cast<uint32_t>(std::span(m_pImpl->m_folderEntries).size_bytes());
	if (strict)
		m_sqpackIndexSubHeader.Sha1.SetFromSpan(&m_sqpackIndexSubHeader, 1);

	memcpy(m_sqpackIndex2Header.Signature, SqpackHeader::Signature_Value, sizeof SqpackHeader::Signature_Value);
	m_sqpackIndex2Header.HeaderSize = sizeof SqpackHeader;
	m_sqpackIndex2Header.Unknown1 = SqpackHeader::Unknown1_Value;
	m_sqpackIndex2Header.Type = SqpackType::SqIndex;
	m_sqpackIndex2Header.Unknown2 = SqpackHeader::Unknown2_Value;
	if (strict)
		m_sqpackIndex2Header.Sha1.SetFromSpan(&m_sqpackIndex2Header, 1);

	m_sqpackIndex2SubHeader.HeaderSize = sizeof SqIndex::Header;
	m_sqpackIndex2SubHeader.Type = SqIndex::Header::IndexType::Index2;
	m_sqpackIndex2SubHeader.FileSegment.Count = 1;
	m_sqpackIndex2SubHeader.FileSegment.Offset = m_sqpackIndex2Header.HeaderSize + m_sqpackIndex2SubHeader.HeaderSize;
	m_sqpackIndex2SubHeader.FileSegment.Size = static_cast<uint32_t>(std::span(m_pImpl->m_fileEntries2).size_bytes());
	m_sqpackIndex2SubHeader.DataFilesSegment.Count = static_cast<uint32_t>(m_sqpackDataSubHeaders.size());
	m_sqpackIndex2SubHeader.DataFilesSegment.Offset = m_sqpackIndex2SubHeader.FileSegment.Offset + m_sqpackIndex2SubHeader.FileSegment.Size;
	m_sqpackIndex2SubHeader.DataFilesSegment.Size = static_cast<uint32_t>(std::span(m_sqpackIndex2Segment2).size_bytes());
	m_sqpackIndex2SubHeader.UnknownSegment3.Count = 0;
	m_sqpackIndex2SubHeader.UnknownSegment3.Offset = m_sqpackIndex2SubHeader.DataFilesSegment.Offset + m_sqpackIndex2SubHeader.DataFilesSegment.Size;
	m_sqpackIndex2SubHeader.UnknownSegment3.Size = static_cast<uint32_t>(std::span(m_sqpackIndex2Segment3).size_bytes());
	m_sqpackIndex2SubHeader.FolderSegment.Count = 0;
	m_sqpackIndex2SubHeader.FolderSegment.Offset = m_sqpackIndex2SubHeader.UnknownSegment3.Offset + m_sqpackIndex2SubHeader.UnknownSegment3.Size;
	m_sqpackIndex2SubHeader.FolderSegment.Size = 0;
	if (strict)
		m_sqpackIndex2SubHeader.Sha1.SetFromSpan(&m_sqpackIndex2SubHeader, 1);

	memcpy(m_sqpackDataHeader.Signature, SqpackHeader::Signature_Value, sizeof SqpackHeader::Signature_Value);
	m_sqpackDataHeader.HeaderSize = sizeof SqpackHeader;
	m_sqpackDataHeader.Unknown1 = SqpackHeader::Unknown1_Value;
	m_sqpackDataHeader.Type = SqpackType::SqData;
	m_sqpackDataHeader.Unknown2 = SqpackHeader::Unknown2_Value;
	if (strict)
		m_sqpackDataHeader.Sha1.SetFromSpan(&m_sqpackDataHeader, 1);

	m_pImpl->m_frozen = true;
}

size_t Sqex::Sqpack::VirtualSqPack::ReadIndex1(const uint64_t offset, void* const buf, const size_t length) const {
	if (!m_pImpl->m_frozen)
		throw std::runtime_error("Trying to operate on a non frozen VirtualSqPack");
	if (!length)
		return 0;

	auto relativeOffset = offset;
	auto out = std::span(static_cast<char*>(buf), length);

	for (const auto& [ptr, cb] : std::initializer_list<std::tuple<const void*, size_t>>{
		{&m_sqpackIndexHeader, sizeof m_sqpackIndexHeader},
		{&m_sqpackIndexSubHeader, sizeof m_sqpackIndexSubHeader},
		{m_pImpl->m_fileEntries1.data(), std::span(m_pImpl->m_fileEntries1).size_bytes()},
		{m_sqpackIndexSegment2.data(), std::span(m_sqpackIndexSegment2).size_bytes()},
		{m_sqpackIndexSegment3.data(), std::span(m_sqpackIndexSegment3).size_bytes()},
		{m_pImpl->m_folderEntries.data(), std::span(m_pImpl->m_folderEntries).size_bytes()},
		}) {
		if (relativeOffset < cb) {
			const auto src = std::span(static_cast<const char*>(ptr), cb)
				.subspan(static_cast<size_t>(relativeOffset));
			const auto available = std::min(out.size_bytes(), src.size_bytes());
			std::copy_n(src.begin(), available, out.begin());
			out = out.subspan(available);
			relativeOffset = 0;
		} else
			relativeOffset -= cb;

		if (out.empty()) return length;
	}

	return length - out.size_bytes();
}

size_t Sqex::Sqpack::VirtualSqPack::ReadIndex2(const uint64_t offset, void* const buf, const size_t length) const {
	if (!m_pImpl->m_frozen)
		throw std::runtime_error("Trying to operate on a non frozen VirtualSqPack");
	if (!length)
		return 0;

	auto relativeOffset = offset;
	auto out = std::span(static_cast<char*>(buf), length);

	for (const auto& [ptr, cb] : std::initializer_list<std::tuple<const void*, size_t>>{
		{&m_sqpackIndex2Header, sizeof m_sqpackIndex2Header},
		{&m_sqpackIndex2SubHeader, sizeof m_sqpackIndex2SubHeader},
		{m_pImpl->m_fileEntries2.data(), std::span(m_pImpl->m_fileEntries2).size_bytes()},
		{m_sqpackIndex2Segment2.data(), std::span(m_sqpackIndex2Segment2).size_bytes()},
		{m_sqpackIndex2Segment3.data(), std::span(m_sqpackIndex2Segment3).size_bytes()},
		}) {
		if (relativeOffset < cb) {
			const auto src = std::span(static_cast<const char*>(ptr), cb)
				.subspan(static_cast<size_t>(relativeOffset));
			const auto available = std::min(out.size_bytes(), src.size_bytes());
			std::copy_n(src.begin(), available, out.begin());
			out = out.subspan(available);
			relativeOffset = 0;
		} else
			relativeOffset -= cb;

		if (out.empty()) return length;
	}

	return length - out.size_bytes();
}

size_t Sqex::Sqpack::VirtualSqPack::ReadData(uint32_t datIndex, const uint64_t offset, void* const buf, const size_t length) const {
	if (!m_pImpl->m_frozen)
		throw std::runtime_error("Trying to operate on a non frozen VirtualSqPack");
	if (!length)
		return 0;

	auto relativeOffset = offset;
	auto out = std::span(static_cast<char*>(buf), length);

	for (const auto& [ptr, cb] : std::initializer_list<std::tuple<const void*, size_t>>{
		{&m_sqpackDataHeader, sizeof m_sqpackDataHeader},
		{&m_sqpackDataSubHeaders[datIndex], sizeof m_sqpackDataSubHeaders[datIndex]},
		}) {
		if (relativeOffset < cb) {
			const auto src = std::span(static_cast<const char*>(ptr), cb)
				.subspan(static_cast<size_t>(relativeOffset));
			const auto available = std::min(out.size_bytes(), src.size_bytes());
			std::copy_n(src.begin(), available, out.begin());
			out = out.subspan(available);
			relativeOffset = 0;
		} else
			relativeOffset -= cb;

		if (out.empty()) return length;
	}

	auto it = std::lower_bound(m_pImpl->m_entries.begin(), m_pImpl->m_entries.end(), nullptr, [&](const std::unique_ptr<Implementation::Entry>& l, const std::unique_ptr<Implementation::Entry>& r) {
		const auto ldfi = l ? l->DataFileIndex : datIndex;
		const auto rdfi = r ? r->DataFileIndex : datIndex;
		if (ldfi == rdfi) {
			const auto lo = l ? l->OffsetAfterHeaders : relativeOffset;
			const auto ro = r ? r->OffsetAfterHeaders : relativeOffset;
			return lo < ro;
		} else
			return ldfi < rdfi;
	});
	if (it != m_pImpl->m_entries.begin() && it != m_pImpl->m_entries.end())
		--it;

	if (it != m_pImpl->m_entries.end()) {
		relativeOffset -= it->get()->OffsetAfterHeaders;

		for (; it < m_pImpl->m_entries.end(); ++it) {
			const auto& entry = *it->get();

			if (relativeOffset < entry.BlockSize) {
				const auto available = std::min(out.size_bytes(), static_cast<size_t>(entry.BlockSize - relativeOffset));
				entry.Provider->ReadStream(relativeOffset, out.data(), available);
				out = out.subspan(available);
				relativeOffset = 0;

				if (out.empty())
					break;
			} else
				relativeOffset -= entry.BlockSize;

			if (relativeOffset < entry.PadSize) {
				const auto available = std::min(out.size_bytes(), static_cast<size_t>(entry.PadSize - relativeOffset));
				std::fill_n(out.begin(), available, 0);
				out = out.subspan(available);
				relativeOffset = 0;

				if (out.empty())
					break;
			} else
				relativeOffset -= entry.PadSize;
		}
	}

	return length - out.size_bytes();
}

uint64_t Sqex::Sqpack::VirtualSqPack::SizeIndex1() const {
	return 0ULL +
		m_sqpackIndexHeader.HeaderSize +
		m_sqpackIndexSubHeader.HeaderSize +
		m_sqpackIndexSubHeader.FileSegment.Size +
		m_sqpackIndexSubHeader.DataFilesSegment.Size +
		m_sqpackIndexSubHeader.UnknownSegment3.Size +
		m_sqpackIndexSubHeader.FolderSegment.Size;
}

uint64_t Sqex::Sqpack::VirtualSqPack::SizeIndex2() const {
	return 0ULL +
		m_sqpackIndex2Header.HeaderSize +
		m_sqpackIndex2SubHeader.HeaderSize +
		m_sqpackIndex2SubHeader.FileSegment.Size +
		m_sqpackIndex2SubHeader.DataFilesSegment.Size +
		m_sqpackIndex2SubHeader.UnknownSegment3.Size +
		m_sqpackIndex2SubHeader.FolderSegment.Size;
}

uint64_t Sqex::Sqpack::VirtualSqPack::SizeData(uint32_t datIndex) const {
	if (datIndex >= m_sqpackDataSubHeaders.size())
		return 0;

	return 0ULL +
		m_sqpackDataHeader.HeaderSize +
		m_sqpackDataSubHeaders[datIndex].HeaderSize +
		m_sqpackDataSubHeaders[datIndex].DataSize;
}
