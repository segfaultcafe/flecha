#ifndef FLECHA_LSP_DOCUMENT_STORE_HPP
#define FLECHA_LSP_DOCUMENT_STORE_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

namespace flecha::lsp {

using DocumentUri = std::string;

struct Document final {
    DocumentUri uri;
    std::string languageId;
    std::int64_t version = 0;
    std::string text;
};

class DocumentStore {
public:
    void Open(const DocumentUri& uri, std::string text, std::int64_t version);
    void Change(const DocumentUri& uri, std::string text, std::int64_t version);
    void Close(const DocumentUri& uri);

    [[nodiscard]] const Document* Find(const DocumentUri& uri) const;

private:
    std::unordered_map<DocumentUri, Document> _documents;
};

}

#endif
