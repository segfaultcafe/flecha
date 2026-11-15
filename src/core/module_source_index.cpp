#include "core/module_source_index.hpp"

#include <algorithm>
#include <system_error>

#include "core/parser.hpp"
#include "core/source_provider.hpp"
#include "core/tokenizer.hpp"

namespace flecha::core {
namespace fs = std::filesystem;
namespace {

fs::path Normalize(const fs::path& path) {
    std::error_code error;
    const fs::path normalized = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

bool IsWithin(const fs::path& path, const fs::path& root) {
    const fs::path relative =
        Normalize(path).lexically_relative(Normalize(root));
    return !relative.empty() && *relative.begin() != "..";
}

std::optional<fs::path> DeclaredTarget(const NamespaceAttachment& attachment,
                                       const fs::path& root) {
    if (attachment.path.empty() || attachment.path.front() != "root")
        return std::nullopt;
    fs::path target = root;
    for (std::size_t index = 1; index < attachment.path.size(); ++index)
        target /= attachment.path[index];
    return target.lexically_normal();
}

}  // namespace

IndexedModuleSource ModuleSourceIndex::Read(const fs::path& inputFile) const {
    const fs::path file = inputFile.lexically_normal();
    IndexedModuleSource result;
    result.file = file;
    const auto source = _sources.Read(file);
    if (!source) return result;

    diagnostics::DiagnosticBag ignored;
    auto tokens = Tokenizer(*source, ignored).Tokenize();
    for (Token& token : tokens) {
        token.range.start.file = file.string();
        token.range.end.file = file.string();
    }
    Parser parser(std::move(tokens), ignored);
    parser.ParseHeader();
    result.name = parser.ModuleName();
    result.namespaceAttachment = parser.NamespaceAttachmentClause();
    return result;
}

NamespaceAttachmentResolution ModuleSourceIndex::ResolveAttachment(
    const fs::path& file, const NamespaceAttachment& attachment,
    const fs::path& inputRoot) {
    const fs::path root = Normalize(inputRoot);
    if (attachment.path.empty())
        return {NamespaceAttachmentStatus::Empty, std::nullopt};
    if (attachment.path.front() != "root")
        return {NamespaceAttachmentStatus::NonRoot, std::nullopt};

    const fs::path declared = *DeclaredTarget(attachment, root);
    if (!fs::is_directory(declared))
        return {NamespaceAttachmentStatus::MissingTarget, declared};

    const fs::path target = Normalize(declared);
    if (!IsWithin(target, root))
        return {NamespaceAttachmentStatus::EscapesRoot, target};

    const fs::path physical = Normalize(file).parent_path();
    const fs::path relative = physical.lexically_relative(target);
    if (relative.empty() || *relative.begin() == "..")
        return {NamespaceAttachmentStatus::NotAncestor, target};
    return {NamespaceAttachmentStatus::Valid, target};
}

ModuleSourceIndex::RootState& ModuleSourceIndex::State(
    const fs::path& inputRoot) {
    const fs::path root = Normalize(inputRoot);
    auto [found, inserted] = _roots.try_emplace(root);
    if (inserted) found->second.root = root;
    return found->second;
}

void ModuleSourceIndex::Remove(RootState& state, const fs::path& file) {
    const fs::path lexical = file.lexically_normal();
    const fs::path normalized = Normalize(file);
    std::erase_if(state.sources, [&](const auto& item) {
        return item.first == lexical || Normalize(item.first) == normalized;
    });
}

void ModuleSourceIndex::Update(RootState& state, const fs::path& file) {
    Remove(state, file);
    if (file.extension() != ".fl") return;
    const IndexedModuleSource source = Read(file);
    if (source.name.empty()) return;
    state.sources.insert_or_assign(source.file.lexically_normal(), source);
}

void ModuleSourceIndex::EnsureAttachments(RootState& state) {
    if (state.attachmentsScanned) return;
    state.attachmentsScanned = true;

    std::error_code scanError;
    fs::recursive_directory_iterator item(
        state.root, fs::directory_options::skip_permission_denied, scanError);
    const fs::recursive_directory_iterator end;
    while (!scanError && item != end) {
        const fs::directory_entry entry = *item;
        item.increment(scanError);
        if (!entry.is_regular_file() || entry.path().extension() != ".fl")
            continue;
        const auto source = _sources.Read(entry.path());
        if (!source || source->find("under") == std::string::npos) continue;
        const IndexedModuleSource indexed = Read(entry.path());
        if (!indexed.name.empty() && indexed.namespaceAttachment)
            state.sources.insert_or_assign(indexed.file.lexically_normal(),
                                           indexed);
    }

    for (const fs::path& file : _sources.KnownSources(state.root))
        Update(state, file);
}

void ModuleSourceIndex::EnsureDirectory(RootState& state,
                                        const fs::path& inputDirectory) {
    const fs::path directory = Normalize(inputDirectory);
    if (!state.scannedDirectories.insert(directory).second) return;

    std::error_code directoryError;
    fs::directory_iterator item(directory, directoryError);
    const fs::directory_iterator end;
    while (!directoryError && item != end) {
        const fs::directory_entry entry = *item;
        item.increment(directoryError);
        if (entry.is_regular_file() && entry.path().extension() == ".fl")
            Update(state, entry.path());
    }
    for (const fs::path& file : _sources.KnownSources(state.root))
        if (Normalize(file).parent_path() == directory) Update(state, file);
}

std::vector<IndexedModuleSource> ModuleSourceIndex::Discover(
    const fs::path& inputRoot, const fs::path& inputDirectory) {
    RootState& state = State(inputRoot);
    const fs::path directory = Normalize(inputDirectory);
    EnsureAttachments(state);
    EnsureDirectory(state, directory);

    std::vector<IndexedModuleSource> result;
    for (const auto& [path, source] : state.sources) {
        if (!source.namespaceAttachment) {
            if (path.parent_path() == directory ||
                Normalize(path).parent_path() == directory)
                result.push_back(source);
            continue;
        }

        const NamespaceAttachmentResolution resolution = ResolveAttachment(
            source.file, *source.namespaceAttachment, state.root);
        if (!resolution.target || Normalize(*resolution.target) != directory)
            continue;
        result.push_back(source);
    }
    std::sort(
        result.begin(), result.end(),
        [](const IndexedModuleSource& left, const IndexedModuleSource& right) {
            const std::string leftPhysical =
                Normalize(left.file).generic_string();
            const std::string rightPhysical =
                Normalize(right.file).generic_string();
            if (leftPhysical != rightPhysical)
                return leftPhysical < rightPhysical;
            return left.file.generic_string() < right.file.generic_string();
        });
    return result;
}

void ModuleSourceIndex::Invalidate(const fs::path& file) {
    for (auto& [root, state] : _roots) {
        const fs::path lexical = file.lexically_normal();
        const fs::path relative = lexical.lexically_relative(root);
        if ((!relative.empty() && *relative.begin() != "..") ||
            IsWithin(file, root))
            Update(state, file);
        else
            Remove(state, file);
    }
}

void ModuleSourceIndex::InvalidateRoot(const fs::path& root) {
    _roots.erase(Normalize(root));
}

}  // namespace flecha::core
